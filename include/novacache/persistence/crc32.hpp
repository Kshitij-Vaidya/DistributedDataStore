#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace novacache::persistence {

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view data) noexcept;
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc,
                                         std::span<const std::byte> data) noexcept;

} // namespace novacache::persistence
