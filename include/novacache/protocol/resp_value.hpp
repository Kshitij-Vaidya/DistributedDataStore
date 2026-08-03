#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace novacache::protocol {

class RespValue;

struct SimpleString {
    std::string value;

    bool operator==(const SimpleString&) const = default;
};

struct Error {
    std::string value;

    bool operator==(const Error&) const = default;
};

struct Integer {
    std::int64_t value{};

    bool operator==(const Integer&) const = default;
};

struct BulkString {
    std::optional<std::string> value;

    bool operator==(const BulkString&) const = default;
};

struct Array {
    std::optional<std::vector<RespValue>> value;

    bool operator==(const Array&) const = default;
};

class RespValue {
  public:
    using Storage = std::variant<SimpleString, Error, Integer, BulkString, Array>;

    RespValue(SimpleString value) : value_(std::move(value)) {}
    RespValue(Error value) : value_(std::move(value)) {}
    RespValue(Integer value) : value_(value) {}
    RespValue(BulkString value) : value_(std::move(value)) {}
    RespValue(Array value) : value_(std::move(value)) {}

    [[nodiscard]] static RespValue simple(std::string value) {
        return SimpleString{std::move(value)};
    }

    [[nodiscard]] static RespValue error(std::string value) { return Error{std::move(value)}; }

    [[nodiscard]] static RespValue integer(std::int64_t value) { return Integer{value}; }

    [[nodiscard]] static RespValue bulk(std::optional<std::string> value) {
        return BulkString{std::move(value)};
    }

    [[nodiscard]] static RespValue array(std::optional<std::vector<RespValue>> value) {
        return Array{std::move(value)};
    }

    [[nodiscard]] const Storage& storage() const noexcept { return value_; }
    [[nodiscard]] Storage& storage() noexcept { return value_; }

    bool operator==(const RespValue&) const = default;

  private:
    Storage value_;
};

} // namespace novacache::protocol
