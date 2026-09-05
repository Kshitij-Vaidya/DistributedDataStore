#include "novacache/persistence/snapshot.hpp"
#include "novacache/store/store.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

class SnapshotTest : public testing::Test {
  protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("novacache-snap-" + std::to_string(::getpid()) + "-" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(directory_);
        path_ = (directory_ / "dump.ncs").string();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::filesystem::path directory_;
    std::string path_;
};

TEST_F(SnapshotTest, RoundTripsEntriesWithExpiry) {
    const auto expiry = novacache::persistence::from_unix_ms(1'700'000'000'000);
    std::vector<novacache::store::PersistedEntry> entries{
        {"alpha", std::string{"one"}, std::nullopt},
        {"beta", std::string{"two"}, expiry},
        {"count", std::int64_t{7}, std::nullopt},
    };

    novacache::persistence::SnapshotStore::write(path_, 42, entries);
    const auto loaded = novacache::persistence::SnapshotStore::read(path_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->first.wal_offset, 42U);
    ASSERT_EQ(loaded->second.size(), 3U);
    EXPECT_EQ(loaded->second[0].key, "alpha");
    EXPECT_EQ(std::get<std::string>(loaded->second[0].value), "one");
    EXPECT_FALSE(loaded->second[0].expiry_wall.has_value());
    EXPECT_EQ(loaded->second[1].key, "beta");
    EXPECT_EQ(loaded->second[1].expiry_wall, expiry);
    EXPECT_EQ(std::get<std::int64_t>(loaded->second[2].value), 7);
}

TEST_F(SnapshotTest, RejectsCorruptChecksum) {
    std::vector<novacache::store::PersistedEntry> entries{
        {"alpha", std::string{"one"}, std::nullopt},
    };
    novacache::persistence::SnapshotStore::write(path_, 1, entries);

    {
        std::fstream file{path_, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(20);
        file.put('X');
    }

    EXPECT_FALSE(novacache::persistence::SnapshotStore::read(path_).has_value());
}

} // namespace
