#include "novacache/store/store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using novacache::store::Clock;
using novacache::store::KeysStatus;
using novacache::store::Store;

class FakeClock final : public Clock {
  public:
    [[nodiscard]] time_point now() const noexcept override { return now_; }

    [[nodiscard]] wall_time_point wall_now() const noexcept override { return wall_now_; }

    void advance(const std::chrono::steady_clock::duration duration) noexcept {
        now_ += duration;
        wall_now_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
    }

    void jump_wall(const std::chrono::system_clock::duration duration) noexcept {
        wall_now_ += duration;
    }

  private:
    time_point now_{};
    wall_time_point wall_now_{};
};

struct ExpiryTest : testing::Test {
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    Store store{4, clock};
};

TEST_F(ExpiryTest, PersistentAndMissingKeysHaveSentinelTtls) {
    EXPECT_EQ(store.ttl("missing"), -2);

    store.set("persistent", std::string{"value"});
    EXPECT_EQ(store.ttl("persistent"), -1);
}

TEST_F(ExpiryTest, ExpireReportsWhetherKeyExists) {
    EXPECT_FALSE(store.expire("missing", 10s));

    store.set("key", std::string{"value"});
    EXPECT_TRUE(store.expire("key", 10s));
    EXPECT_EQ(store.ttl("key"), 10);
}

TEST_F(ExpiryTest, TtlUsesWholeSecondsRoundedDown) {
    store.set("key", std::string{"value"});
    ASSERT_TRUE(store.expire("key", 10s));

    clock->advance(1500ms);

    EXPECT_EQ(store.ttl("key"), 8);
    EXPECT_TRUE(store.exists("key"));
}

TEST_F(ExpiryTest, KeyExpiresExactlyAtDeadline) {
    store.set("key", std::int64_t{1});
    ASSERT_TRUE(store.expire("key", 2s));

    clock->advance(1999ms);
    EXPECT_EQ(store.ttl("key"), 0);
    EXPECT_TRUE(store.exists("key"));

    clock->advance(1ms);
    EXPECT_EQ(store.ttl("key"), -2);
    EXPECT_FALSE(store.get("key").has_value());
}

TEST_F(ExpiryTest, GetLazilyRemovesExpiredKey) {
    store.set("key", std::string{"value"});
    ASSERT_TRUE(store.expire("key", 1s));
    clock->advance(1s);

    EXPECT_FALSE(store.get("key").has_value());
    EXPECT_FALSE(store.exists("key"));
}

TEST_F(ExpiryTest, DelTreatsExpiredKeyAsMissing) {
    store.set("key", std::string{"value"});
    ASSERT_TRUE(store.expire("key", 1s));
    clock->advance(1s);

    EXPECT_FALSE(store.del("key"));
    EXPECT_EQ(store.ttl("key"), -2);
}

TEST_F(ExpiryTest, SetReplacesExpiredEntryWithPersistentValue) {
    store.set("key", std::string{"old"});
    ASSERT_TRUE(store.expire("key", 1s));
    clock->advance(1s);

    store.set("key", std::string{"new"});

    EXPECT_EQ(store.ttl("key"), -1);
    const auto value = store.get("key");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::get<std::string>(*value), "new");
}

TEST_F(ExpiryTest, SetClearsExistingExpiry) {
    store.set("key", std::string{"old"});
    ASSERT_TRUE(store.expire("key", 10s));

    store.set("key", std::string{"new"});
    clock->advance(20s);

    EXPECT_TRUE(store.exists("key"));
    EXPECT_EQ(store.ttl("key"), -1);
}

TEST_F(ExpiryTest, ReapplyingExpireReplacesDeadline) {
    store.set("key", std::string{"value"});
    ASSERT_TRUE(store.expire("key", 2s));
    clock->advance(1s);
    ASSERT_TRUE(store.expire("key", 5s));
    clock->advance(2s);

    EXPECT_EQ(store.ttl("key"), 3);
    EXPECT_TRUE(store.exists("key"));
}

TEST_F(ExpiryTest, NonPositiveExpiryDeletesExistingKeyImmediately) {
    store.set("zero", std::string{"value"});
    store.set("negative", std::string{"value"});

    EXPECT_TRUE(store.expire("zero", 0s));
    EXPECT_TRUE(store.expire("negative", -1s));
    EXPECT_FALSE(store.exists("zero"));
    EXPECT_FALSE(store.exists("negative"));
}

TEST_F(ExpiryTest, KeysLazilyRemovesExpiredEntriesAcrossShards) {
    store.set("persistent", std::string{"value"});
    store.set("short", std::string{"value"});
    store.set("long", std::string{"value"});
    ASSERT_TRUE(store.expire("short", 1s));
    ASSERT_TRUE(store.expire("long", 3s));
    clock->advance(2s);

    const auto result = store.keys("*");

    EXPECT_EQ(result.status, KeysStatus::ok);
    EXPECT_EQ(result.keys, (std::vector<std::string>{"long", "persistent"}));
    EXPECT_EQ(store.ttl("short"), -2);
}

TEST_F(ExpiryTest, RecordsAbsoluteWallClockExpiryForSerialization) {
    store.set("persistent", std::string{"value"});
    EXPECT_FALSE(store.expiry_wall_time("persistent").has_value());
    EXPECT_FALSE(store.expiry_wall_time("missing").has_value());

    store.set("key", std::string{"value"});
    const auto before = clock->wall_now();
    ASSERT_TRUE(store.expire("key", 10s));
    const auto wall = store.expiry_wall_time("key");
    ASSERT_TRUE(wall.has_value());
    EXPECT_EQ(*wall, before + 10s);

    // Wall jumps must not change runtime TTL; serialization stamp stays fixed.
    clock->jump_wall(1h);
    EXPECT_EQ(store.ttl("key"), 10);
    EXPECT_EQ(store.expiry_wall_time("key"), before + 10s);

    store.set("key", std::string{"replaced"});
    EXPECT_FALSE(store.expiry_wall_time("key").has_value());
}

} // namespace
