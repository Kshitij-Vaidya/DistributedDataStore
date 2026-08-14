#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace novacache::net {

enum class Interest : std::uint8_t {
    none = 0,
    read = 1,
    write = 2,
};

[[nodiscard]] constexpr Interest operator|(const Interest left, const Interest right) noexcept {
    return static_cast<Interest>(static_cast<std::uint8_t>(left) |
                                 static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr Interest operator&(const Interest left, const Interest right) noexcept {
    return static_cast<Interest>(static_cast<std::uint8_t>(left) &
                                 static_cast<std::uint8_t>(right));
}

constexpr Interest& operator|=(Interest& left, const Interest right) noexcept {
    left = left | right;
    return left;
}

constexpr Interest& operator&=(Interest& left, const Interest right) noexcept {
    left = left & right;
    return left;
}

[[nodiscard]] constexpr bool has_interest(const Interest value, const Interest flag) noexcept {
    return (value & flag) != Interest::none;
}

struct FiredEvent {
    int fd = -1;
    Interest ready = Interest::none;
    bool error = false;
    bool hangup = false;
};

class Reactor {
  public:
    virtual ~Reactor() = default;

    virtual void add(int fd, Interest interest) = 0;
    virtual void modify(int fd, Interest interest) = 0;
    virtual void remove(int fd) = 0;
    virtual std::size_t wait(std::span<FiredEvent> out,
                             std::optional<std::chrono::milliseconds> timeout) = 0;
    virtual void wakeup() = 0;
};

[[nodiscard]] std::unique_ptr<Reactor> create_reactor();

} // namespace novacache::net
