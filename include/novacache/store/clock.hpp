#pragma once

#include <chrono>

namespace novacache::store {

class Clock {
  public:
    using time_point = std::chrono::steady_clock::time_point;
    using wall_time_point = std::chrono::system_clock::time_point;

    virtual ~Clock() = default;

    // Runtime deadlines use a monotonic clock so TTL is not affected by wall jumps.
    [[nodiscard]] virtual time_point now() const noexcept = 0;

    // Absolute wall-clock instants are retained for persistence serialization.
    [[nodiscard]] virtual wall_time_point wall_now() const noexcept = 0;
};

class SystemClock final : public Clock {
  public:
    [[nodiscard]] time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }

    [[nodiscard]] wall_time_point wall_now() const noexcept override {
        return std::chrono::system_clock::now();
    }
};

} // namespace novacache::store
