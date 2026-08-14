#pragma once

#include "novacache/store/clock.hpp"
#include "novacache/store/value.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace novacache::store {

enum class KeysStatus {
    ok,
    unsupported_pattern,
};

struct KeysResult {
    KeysStatus status{KeysStatus::ok};
    std::vector<std::string> keys;
};

class Store {
  public:
    static constexpr std::size_t default_shard_count = 16;

    explicit Store(std::size_t shard_count = default_shard_count,
                   std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());

    void set(std::string key, Value value);
    [[nodiscard]] std::optional<Value> get(std::string_view key);
    [[nodiscard]] bool del(std::string_view key);
    [[nodiscard]] std::size_t del_many(const std::vector<std::string_view>& keys);
    [[nodiscard]] bool exists(std::string_view key);
    [[nodiscard]] std::size_t exists_many(const std::vector<std::string_view>& keys);
    [[nodiscard]] bool expire(std::string_view key, std::chrono::seconds ttl);
    [[nodiscard]] std::int64_t ttl(std::string_view key);
    [[nodiscard]] KeysResult keys(std::string_view pattern);

    // Returns the absolute wall-clock expiry used for future persistence, if any.
    [[nodiscard]] std::optional<Clock::wall_time_point> expiry_wall_time(std::string_view key);

    // Samples up to `samples_per_shard` random entries per shard and erases expired keys.
    [[nodiscard]] std::size_t active_expire_cycle(std::size_t samples_per_shard);

    [[nodiscard]] std::size_t shard_count() const noexcept;
    [[nodiscard]] std::uint64_t approximate_memory() const noexcept;
    [[nodiscard]] std::uint64_t expired_keys() const noexcept;

  private:
    struct Entry {
        Value value;
        std::optional<Clock::time_point> expires_at;
        std::optional<Clock::wall_time_point> expires_at_wall;
        std::size_t stored_bytes = 0;
    };

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, Entry> entries;
    };

    [[nodiscard]] std::size_t shard_index(std::string_view key) const noexcept;
    [[nodiscard]] Shard& shard_for(std::string_view key) noexcept;
    [[nodiscard]] static std::size_t estimate_bytes(std::string_view key, const Value& value);
    [[nodiscard]] bool is_expired(const Entry& entry, Clock::time_point now) const noexcept;
    bool erase_expired_locked(Shard& shard,
                              std::unordered_map<std::string, Entry>::iterator iterator,
                              Clock::time_point now);

    std::vector<Shard> shards_;
    std::shared_ptr<const Clock> clock_;
    std::atomic<std::uint64_t> approximate_memory_{0};
    std::atomic<std::uint64_t> expired_keys_{0};
};

} // namespace novacache::store
