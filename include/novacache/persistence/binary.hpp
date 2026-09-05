#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace novacache::persistence {

inline void append_u8(std::string& out, const std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

inline void append_u16(std::string& out, const std::uint16_t value) {
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xffU),
                                   static_cast<std::uint8_t>((value >> 8U) & 0xffU)};
    out.append(reinterpret_cast<const char*>(bytes), 2);
}

inline void append_u32(std::string& out, const std::uint32_t value) {
    const std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
    };
    out.append(reinterpret_cast<const char*>(bytes), 4);
}

inline void append_u64(std::string& out, const std::uint64_t value) {
    const std::uint8_t bytes[8] = {
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
        static_cast<std::uint8_t>((value >> 32U) & 0xffU),
        static_cast<std::uint8_t>((value >> 40U) & 0xffU),
        static_cast<std::uint8_t>((value >> 48U) & 0xffU),
        static_cast<std::uint8_t>((value >> 56U) & 0xffU),
    };
    out.append(reinterpret_cast<const char*>(bytes), 8);
}

inline void append_i64(std::string& out, const std::int64_t value) {
    append_u64(out, static_cast<std::uint64_t>(value));
}

inline void append_bytes(std::string& out, const std::string_view value) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
}

[[nodiscard]] inline std::uint8_t read_u8(std::string_view& input) {
    if (input.size() < 1U) {
        throw std::runtime_error("truncated input while reading u8");
    }
    const auto value = static_cast<std::uint8_t>(input[0]);
    input.remove_prefix(1);
    return value;
}

[[nodiscard]] inline std::uint16_t read_u16(std::string_view& input) {
    if (input.size() < 2U) {
        throw std::runtime_error("truncated input while reading u16");
    }
    const auto value = static_cast<std::uint16_t>(static_cast<std::uint8_t>(input[0]) |
                                                  (static_cast<std::uint16_t>(
                                                       static_cast<std::uint8_t>(input[1]))
                                                   << 8U));
    input.remove_prefix(2);
    return value;
}

[[nodiscard]] inline std::uint32_t read_u32(std::string_view& input) {
    if (input.size() < 4U) {
        throw std::runtime_error("truncated input while reading u32");
    }
    const auto value = static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[0])) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[1])) << 8U) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[2])) << 16U) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[3])) << 24U);
    input.remove_prefix(4);
    return value;
}

[[nodiscard]] inline std::uint64_t read_u64(std::string_view& input) {
    if (input.size() < 8U) {
        throw std::runtime_error("truncated input while reading u64");
    }
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(input[static_cast<std::size_t>(index)]))
                 << (8 * index);
    }
    input.remove_prefix(8);
    return value;
}

[[nodiscard]] inline std::int64_t read_i64(std::string_view& input) {
    return static_cast<std::int64_t>(read_u64(input));
}

[[nodiscard]] inline std::string read_bytes(std::string_view& input) {
    const std::uint32_t length = read_u32(input);
    if (input.size() < length) {
        throw std::runtime_error("truncated input while reading bytes");
    }
    std::string value{input.substr(0, length)};
    input.remove_prefix(length);
    return value;
}

} // namespace novacache::persistence
