#pragma once

#include "novacache/store/clock.hpp"
#include "novacache/store/value.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
    [[nodiscard]] bool exists(std::string_view key);
    [[nodiscard]] bool expire(std::string_view key, std::chrono::seconds ttl);
    [[nodiscard]] std::int64_t ttl(std::string_view key);
    [[nodiscard]] KeysResult keys(std::string_view pattern);

    // Returns the absolute wall-clock expiry used for future persistence, if any.
    [[nodiscard]] std::optional<Clock::wall_time_point> expiry_wall_time(std::string_view key);

    [[nodiscard]] std::size_t shard_count() const noexcept;

  private:
    struct Entry {
        Value value;
        std::optional<Clock::time_point> expires_at;
        std::optional<Clock::wall_time_point> expires_at_wall;
    };

    using Shard = std::unordered_map<std::string, Entry>;

    [[nodiscard]] std::size_t shard_index(std::string_view key) const noexcept;
    [[nodiscard]] Shard& shard_for(std::string_view key) noexcept;
    [[nodiscard]] bool erase_if_expired(Shard& shard, std::string_view key);

    std::vector<Shard> shards_;
    std::shared_ptr<const Clock> clock_;
};

} // namespace novacache::store
