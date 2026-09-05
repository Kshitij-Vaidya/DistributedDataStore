#include "novacache/persistence/crc32.hpp"

#include <array>

namespace novacache::persistence {
namespace {

[[nodiscard]] const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t crc = index;
            for (int bit = 0; bit < 8; ++bit) {
                if ((crc & 1U) != 0U) {
                    crc = 0xEDB88320U ^ (crc >> 1U);
                } else {
                    crc >>= 1U;
                }
            }
            values[index] = crc;
        }
        return values;
    }();
    return table;
}

} // namespace

std::uint32_t crc32_update(std::uint32_t crc, const std::span<const std::byte> data) noexcept {
    crc = ~crc;
    const auto& table = crc_table();
    for (const std::byte byte : data) {
        const auto index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint8_t>(byte));
        crc = table[index] ^ (crc >> 8U);
    }
    return ~crc;
}

std::uint32_t crc32(const std::span<const std::byte> data) noexcept {
    return crc32_update(0, data);
}

std::uint32_t crc32(const std::string_view data) noexcept {
    return crc32(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                            data.size()));
}

} // namespace novacache::persistence
