#include "novacache/store/store.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <random>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace novacache::store {
namespace {

[[nodiscard]] std::vector<std::unique_lock<std::shared_mutex>>
lock_mutexes_in_order(std::vector<std::shared_mutex*> mutexes) {
    std::ranges::sort(mutexes);
    mutexes.erase(std::unique(mutexes.begin(), mutexes.end()), mutexes.end());
    std::vector<std::unique_lock<std::shared_mutex>> locks;
    locks.reserve(mutexes.size());
    for (std::shared_mutex* mutex : mutexes) {
        locks.emplace_back(*mutex);
    }
    return locks;
}

} // namespace

Store::Store(const std::size_t shard_count, std::shared_ptr<const Clock> clock)
    : shards_(shard_count), clock_(std::move(clock)) {
    if (shard_count == 0U) {
        throw std::invalid_argument("store shard count must be greater than zero");
    }
    if (clock_ == nullptr) {
        throw std::invalid_argument("store clock must not be null");
    }
}

std::size_t Store::estimate_bytes(const std::string_view key, const Value& value) {
    std::size_t total = key.size() + sizeof(Entry);
    std::visit(
        [&total](const auto& stored) {
            using Stored = std::decay_t<decltype(stored)>;
            if constexpr (std::is_same_v<Stored, std::string>) {
                total += stored.size();
            } else if constexpr (std::is_same_v<Stored, std::int64_t>) {
                total += sizeof(std::int64_t);
            } else if constexpr (std::is_same_v<Stored, StringList>) {
                for (const std::string& item : stored) {
                    total += item.size();
                }
            } else if constexpr (std::is_same_v<Stored, StringSet>) {
                for (const std::string& item : stored) {
                    total += item.size();
                }
            }
        },
        value);
    return total;
}

bool Store::is_expired(const Entry& entry, const Clock::time_point now) const noexcept {
    return entry.expires_at.has_value() && *entry.expires_at <= now;
}

bool Store::erase_expired_locked(Shard& shard,
                                 const std::unordered_map<std::string, Entry>::iterator iterator,
                                 const Clock::time_point now) {
    if (!is_expired(iterator->second, now)) {
        return false;
    }
    approximate_memory_.fetch_sub(iterator->second.stored_bytes, std::memory_order_relaxed);
    shard.entries.erase(iterator);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Store::set(std::string key, Value value) {
    Shard& shard = shard_for(key);
    const std::size_t bytes = estimate_bytes(key, value);
    const std::unique_lock lock{shard.mutex};
    const auto now = clock_->now();
    const auto iterator = shard.entries.find(key);
    if (iterator != shard.entries.end()) {
        if (is_expired(iterator->second, now)) {
            expired_keys_.fetch_add(1, std::memory_order_relaxed);
        }
        approximate_memory_.fetch_sub(iterator->second.stored_bytes, std::memory_order_relaxed);
        iterator->second = Entry{std::move(value), std::nullopt, std::nullopt, bytes};
    } else {
        shard.entries.emplace(std::move(key),
                              Entry{std::move(value), std::nullopt, std::nullopt, bytes});
    }
    approximate_memory_.fetch_add(bytes, std::memory_order_relaxed);
}

std::optional<Value> Store::get(const std::string_view key) {
    Shard& shard = shard_for(key);
    const auto now = clock_->now();
    {
        const std::shared_lock lock{shard.mutex};
        const auto iterator = shard.entries.find(std::string{key});
        if (iterator == shard.entries.end()) {
            return std::nullopt;
        }
        if (!is_expired(iterator->second, now)) {
            return iterator->second.value;
        }
    }

    const std::unique_lock lock{shard.mutex};
    const auto iterator = shard.entries.find(std::string{key});
    if (iterator == shard.entries.end()) {
        return std::nullopt;
    }
    if (erase_expired_locked(shard, iterator, now)) {
        return std::nullopt;
    }
    return iterator->second.value;
}

bool Store::del(const std::string_view key) {
    Shard& shard = shard_for(key);
    const std::unique_lock lock{shard.mutex};
    const auto iterator = shard.entries.find(std::string{key});
    if (iterator == shard.entries.end()) {
        return false;
    }
    if (erase_expired_locked(shard, iterator, clock_->now())) {
        return false;
    }
    approximate_memory_.fetch_sub(iterator->second.stored_bytes, std::memory_order_relaxed);
    shard.entries.erase(iterator);
    return true;
}

std::size_t Store::del_many(const std::vector<std::string_view>& keys) {
    if (keys.empty()) {
        return 0;
    }
    std::vector<std::shared_mutex*> mutexes;
    mutexes.reserve(keys.size());
    for (const std::string_view key : keys) {
        mutexes.push_back(&shard_for(key).mutex);
    }
    const auto locks = lock_mutexes_in_order(std::move(mutexes));
    const auto now = clock_->now();
    std::size_t removed = 0;
    for (const std::string_view key : keys) {
        Shard& shard = shard_for(key);
        const auto iterator = shard.entries.find(std::string{key});
        if (iterator == shard.entries.end()) {
            continue;
        }
        if (erase_expired_locked(shard, iterator, now)) {
            continue;
        }
        approximate_memory_.fetch_sub(iterator->second.stored_bytes, std::memory_order_relaxed);
        shard.entries.erase(iterator);
        ++removed;
    }
    return removed;
}

bool Store::exists(const std::string_view key) {
    Shard& shard = shard_for(key);
    const auto now = clock_->now();
    {
        const std::shared_lock lock{shard.mutex};
        const auto iterator = shard.entries.find(std::string{key});
        if (iterator == shard.entries.end()) {
            return false;
        }
        if (!is_expired(iterator->second, now)) {
            return true;
        }
    }
    const std::unique_lock lock{shard.mutex};
    const auto iterator = shard.entries.find(std::string{key});
    if (iterator == shard.entries.end()) {
        return false;
    }
    return !erase_expired_locked(shard, iterator, now);
}

std::size_t Store::exists_many(const std::vector<std::string_view>& keys) {
    if (keys.empty()) {
        return 0;
    }
    std::vector<std::shared_mutex*> mutexes;
    mutexes.reserve(keys.size());
    for (const std::string_view key : keys) {
        mutexes.push_back(&shard_for(key).mutex);
    }
    const auto locks = lock_mutexes_in_order(std::move(mutexes));
    const auto now = clock_->now();
    std::size_t count = 0;
    for (const std::string_view key : keys) {
        Shard& shard = shard_for(key);
        const auto iterator = shard.entries.find(std::string{key});
        if (iterator == shard.entries.end()) {
            continue;
        }
        if (erase_expired_locked(shard, iterator, now)) {
            continue;
        }
        ++count;
    }
    return count;
}

bool Store::expire(const std::string_view key, const std::chrono::seconds ttl) {
    Shard& shard = shard_for(key);
    const std::unique_lock lock{shard.mutex};
    const auto iterator = shard.entries.find(std::string{key});
    if (iterator == shard.entries.end()) {
        return false;
    }
    if (erase_expired_locked(shard, iterator, clock_->now())) {
        return false;
    }
    if (ttl <= std::chrono::seconds::zero()) {
        approximate_memory_.fetch_sub(iterator->second.stored_bytes, std::memory_order_relaxed);
        shard.entries.erase(iterator);
        return true;
    }

    iterator->second.expires_at = clock_->now() + ttl;
    iterator->second.expires_at_wall = clock_->wall_now() + ttl;
    return true;
}

std::int64_t Store::ttl(const std::string_view key) {
    Shard& shard = shard_for(key);
    const auto now = clock_->now();
    {
        const std::shared_lock lock{shard.mutex};
        const auto iterator = shard.entries.find(std::string{key});
        if (iterator == shard.entries.end()) {
            return -2;
        }
        if (!iterator->second.expires_at.has_value()) {
            return -1;
        }
        if (*iterator->second.expires_at > now) {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(*iterator->second.expires_at - now)
                    .count());
        }
    }

    const std::unique_lock lock{shard.mutex};
    const auto iterator = shard.entries.find(std::string{key});
    if (iterator == shard.entries.end()) {
        return -2;
    }
    if (!iterator->second.expires_at.has_value()) {
        return -1;
    }
    if (erase_expired_locked(shard, iterator, now)) {
        return -2;
    }
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(*iterator->second.expires_at - now)
            .count());
}

KeysResult Store::keys(const std::string_view pattern) {
    if (pattern != "*") {
        return KeysResult{KeysStatus::unsupported_pattern, {}};
    }

    std::vector<std::string> result;
    const auto now = clock_->now();
    for (Shard& shard : shards_) {
        std::vector<std::string> snapshot;
        {
            const std::unique_lock lock{shard.mutex};
            for (auto iterator = shard.entries.begin(); iterator != shard.entries.end();) {
                if (is_expired(iterator->second, now)) {
                    approximate_memory_.fetch_sub(iterator->second.stored_bytes,
                                                  std::memory_order_relaxed);
                    iterator = shard.entries.erase(iterator);
                    expired_keys_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    snapshot.push_back(iterator->first);
                    ++iterator;
                }
            }
        }
        result.insert(result.end(), std::make_move_iterator(snapshot.begin()),
                      std::make_move_iterator(snapshot.end()));
    }
    std::ranges::sort(result);
    return KeysResult{KeysStatus::ok, std::move(result)};
}

std::optional<Clock::wall_time_point> Store::expiry_wall_time(const std::string_view key) {
    Shard& shard = shard_for(key);
    const auto now = clock_->now();
    {
        const std::shared_lock lock{shard.mutex};
        const auto iterator = shard.entries.find(std::string{key});
        if (iterator == shard.entries.end()) {
            return std::nullopt;
        }
        if (!is_expired(iterator->second, now)) {
            return iterator->second.expires_at_wall;
        }
    }
    const std::unique_lock lock{shard.mutex};
    const auto iterator = shard.entries.find(std::string{key});
    if (iterator == shard.entries.end()) {
        return std::nullopt;
    }
    if (erase_expired_locked(shard, iterator, now)) {
        return std::nullopt;
    }
    return iterator->second.expires_at_wall;
}

std::size_t Store::active_expire_cycle(const std::size_t samples_per_shard) {
    if (samples_per_shard == 0U) {
        return 0;
    }

    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::size_t expired = 0;
    const auto now = clock_->now();

    for (Shard& shard : shards_) {
        const std::unique_lock lock{shard.mutex};
        if (shard.entries.empty()) {
            continue;
        }
        const std::size_t samples = std::min(samples_per_shard, shard.entries.size());
        for (std::size_t attempt = 0; attempt < samples; ++attempt) {
            std::uniform_int_distribution<std::size_t> dist{0, shard.entries.size() - 1U};
            auto iterator = shard.entries.begin();
            std::advance(iterator, static_cast<std::ptrdiff_t>(dist(rng)));
            if (erase_expired_locked(shard, iterator, now)) {
                ++expired;
            }
        }
    }
    return expired;
}

std::size_t Store::shard_count() const noexcept { return shards_.size(); }

std::uint64_t Store::approximate_memory() const noexcept {
    return approximate_memory_.load(std::memory_order_relaxed);
}

std::uint64_t Store::expired_keys() const noexcept {
    return expired_keys_.load(std::memory_order_relaxed);
}

std::size_t Store::shard_index(const std::string_view key) const noexcept {
    return std::hash<std::string_view>{}(key) % shards_.size();
}

Store::Shard& Store::shard_for(const std::string_view key) noexcept {
    return shards_[shard_index(key)];
}

} // namespace novacache::store
