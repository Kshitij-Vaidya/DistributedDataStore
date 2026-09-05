#include "novacache/persistence/crc32.hpp"
#include "novacache/persistence/wal.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

class WalTest : public testing::Test {
  protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("novacache-wal-" + std::to_string(::getpid()) + "-" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(directory_);
        path_ = (directory_ / "append.ncwal").string();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::filesystem::path directory_;
    std::string path_;
};

TEST_F(WalTest, AppendsAndRecoversRecordsInOrder) {
    {
        novacache::persistence::WalWriter writer{path_};
        ASSERT_TRUE(writer.open_and_recover().empty());
        EXPECT_EQ(writer.append(novacache::persistence::WalOpcode::set,
                                novacache::persistence::encode_set_payload("a", "1", std::nullopt)),
                  1U);
        EXPECT_EQ(writer.append(novacache::persistence::WalOpcode::del,
                                novacache::persistence::encode_del_payload({"a"})),
                  2U);
        writer.sync();
    }

    novacache::persistence::WalWriter writer{path_};
    const auto records = writer.open_and_recover();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].offset, 1U);
    EXPECT_EQ(records[0].opcode, novacache::persistence::WalOpcode::set);
    EXPECT_EQ(records[1].offset, 2U);
    EXPECT_EQ(records[1].opcode, novacache::persistence::WalOpcode::del);
    EXPECT_EQ(writer.last_offset(), 2U);
}

TEST_F(WalTest, TruncatesCorruptIncompleteTail) {
    {
        novacache::persistence::WalWriter writer{path_};
        ASSERT_TRUE(writer.open_and_recover().empty());
        static_cast<void>(writer.append(
            novacache::persistence::WalOpcode::set,
            novacache::persistence::encode_set_payload("keep", "value", std::nullopt)));
        writer.sync();
    }

    {
        std::ofstream out{path_, std::ios::binary | std::ios::app};
        out << "broken-tail";
    }

    novacache::persistence::WalWriter writer{path_};
    const auto records = writer.open_and_recover();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].offset, 1U);
    EXPECT_EQ(writer.last_offset(), 1U);
}

TEST_F(WalTest, Crc32IsStableForKnownInput) {
    EXPECT_EQ(novacache::persistence::crc32(std::string_view{"123456789"}), 0xCBF43926U);
}

} // namespace
