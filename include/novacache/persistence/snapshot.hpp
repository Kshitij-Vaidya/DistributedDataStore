#pragma once

#include "novacache/store/store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace novacache::persistence {

struct SnapshotHeader {
    std::uint64_t wal_offset = 0;
    std::uint64_t entry_count = 0;
};

class SnapshotStore {
  public:
    static constexpr std::string_view magic{"NCSNP\0\0\0", 8};
    static constexpr std::uint32_t version = 1;

    static void write(std::string_view path, std::uint64_t wal_offset,
                      const std::vector<store::PersistedEntry>& entries);

    [[nodiscard]] static std::optional<
        std::pair<SnapshotHeader, std::vector<store::PersistedEntry>>>
    read(std::string_view path);
};

[[nodiscard]] std::int64_t to_unix_ms(store::Clock::wall_time_point time);
[[nodiscard]] store::Clock::wall_time_point from_unix_ms(std::int64_t millis);

} // namespace novacache::persistence
