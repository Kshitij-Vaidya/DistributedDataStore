#include "novacache/store/store.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

namespace novacache::store {

Store::Store(const std::size_t shard_count, std::shared_ptr<const Clock> clock)
    : shards_(shard_count), clock_(std::move(clock)) {
    if (shard_count == 0U) {
        throw std::invalid_argument("store shard count must be greater than zero");
    }
    if (clock_ == nullptr) {
        throw std::invalid_argument("store clock must not be null");
    }
}

void Store::set(std::string key, Value value) {
    Shard& shard = shard_for(key);
    static_cast<void>(erase_if_expired(shard, key));
    shard.insert_or_assign(std::move(key), Entry{std::move(value), std::nullopt, std::nullopt});
}

std::optional<Value> Store::get(const std::string_view key) {
    Shard& shard = shard_for(key);
    if (erase_if_expired(shard, key)) {
        return std::nullopt;
    }

    const auto iterator = shard.find(std::string{key});
    if (iterator == shard.end()) {
        return std::nullopt;
    }
    return iterator->second.value;
}

bool Store::del(const std::string_view key) {
    Shard& shard = shard_for(key);
    if (erase_if_expired(shard, key)) {
        return false;
    }
    return shard.erase(std::string{key}) != 0U;
}

bool Store::exists(const std::string_view key) {
    Shard& shard = shard_for(key);
    if (erase_if_expired(shard, key)) {
        return false;
    }
    return shard.contains(std::string{key});
}

bool Store::expire(const std::string_view key, const std::chrono::seconds ttl) {
    Shard& shard = shard_for(key);
    if (erase_if_expired(shard, key)) {
        return false;
    }

    const auto iterator = shard.find(std::string{key});
    if (iterator == shard.end()) {
        return false;
    }
    if (ttl <= std::chrono::seconds::zero()) {
        shard.erase(iterator);
        return true;
    }

    iterator->second.expires_at = clock_->now() + ttl;
    iterator->second.expires_at_wall = clock_->wall_now() + ttl;
    return true;
}

std::int64_t Store::ttl(const std::string_view key) {
    Shard& shard = shard_for(key);
    const auto iterator = shard.find(std::string{key});
    if (iterator == shard.end()) {
        return -2;
    }
    if (!iterator->second.expires_at.has_value()) {
        return -1;
    }

    const auto now = clock_->now();
    if (*iterator->second.expires_at <= now) {
        shard.erase(iterator);
        return -2;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::seconds>(*iterator->second.expires_at - now);
    return static_cast<std::int64_t>(remaining.count());
}

KeysResult Store::keys(const std::string_view pattern) {
    if (pattern != "*") {
        return KeysResult{KeysStatus::unsupported_pattern, {}};
    }

    std::vector<std::string> result;
    const auto now = clock_->now();
    for (Shard& shard : shards_) {
        for (auto iterator = shard.begin(); iterator != shard.end();) {
            const auto& expiry = iterator->second.expires_at;
            if (expiry.has_value() && *expiry <= now) {
                iterator = shard.erase(iterator);
            } else {
                result.push_back(iterator->first);
                ++iterator;
            }
        }
    }
    std::ranges::sort(result);
    return KeysResult{KeysStatus::ok, std::move(result)};
}

std::optional<Clock::wall_time_point> Store::expiry_wall_time(const std::string_view key) {
    Shard& shard = shard_for(key);
    if (erase_if_expired(shard, key)) {
        return std::nullopt;
    }

    const auto iterator = shard.find(std::string{key});
    if (iterator == shard.end()) {
        return std::nullopt;
    }
    return iterator->second.expires_at_wall;
}

std::size_t Store::shard_count() const noexcept { return shards_.size(); }

std::size_t Store::shard_index(const std::string_view key) const noexcept {
    return std::hash<std::string_view>{}(key) % shards_.size();
}

Store::Shard& Store::shard_for(const std::string_view key) noexcept {
    return shards_[shard_index(key)];
}

bool Store::erase_if_expired(Shard& shard, const std::string_view key) {
    const auto iterator = shard.find(std::string{key});
    if (iterator == shard.end()) {
        return false;
    }

    const auto& expiry = iterator->second.expires_at;
    if (expiry.has_value() && *expiry <= clock_->now()) {
        shard.erase(iterator);
        return true;
    }
    return false;
}

} // namespace novacache::store
