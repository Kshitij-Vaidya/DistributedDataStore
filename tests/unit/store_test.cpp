#include "novacache/store/store.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using novacache::store::KeysStatus;
using novacache::store::Store;
using novacache::store::StringList;
using novacache::store::StringSet;
using novacache::store::Value;

TEST(StoreTest, UsesDefaultAndConfigurableFixedShardCounts) {
    const Store default_store;
    const Store configured_store{3};

    EXPECT_EQ(default_store.shard_count(), Store::default_shard_count);
    EXPECT_EQ(configured_store.shard_count(), 3U);
}

TEST(StoreTest, RejectsInvalidConstruction) {
    EXPECT_THROW(static_cast<void>(Store{0}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(Store{1, nullptr}), std::invalid_argument);
}

TEST(StoreTest, StoresEverySupportedValueType) {
    Store store;
    store.set("string", std::string{"value"});
    store.set("integer", std::int64_t{42});
    store.set("list", StringList{"first", "second"});
    store.set("set", StringSet{"red", "blue"});

    ASSERT_TRUE(store.get("string").has_value());
    EXPECT_EQ(std::get<std::string>(*store.get("string")), "value");
    ASSERT_TRUE(store.get("integer").has_value());
    EXPECT_EQ(std::get<std::int64_t>(*store.get("integer")), 42);
    ASSERT_TRUE(store.get("list").has_value());
    EXPECT_EQ(std::get<StringList>(*store.get("list")), (StringList{"first", "second"}));
    ASSERT_TRUE(store.get("set").has_value());
    EXPECT_EQ(std::get<StringSet>(*store.get("set")), (StringSet{"red", "blue"}));
}

TEST(StoreTest, SetOverwritesExistingValue) {
    Store store;
    store.set("key", std::string{"old"});
    store.set("key", std::int64_t{7});

    const auto value = store.get("key");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(*value), 7);
}

TEST(StoreTest, GetDelAndExistsReportMissingKeys) {
    Store store;

    EXPECT_FALSE(store.get("missing").has_value());
    EXPECT_FALSE(store.exists("missing"));
    EXPECT_FALSE(store.del("missing"));

    store.set("present", std::string{"value"});
    EXPECT_TRUE(store.exists("present"));
    EXPECT_TRUE(store.del("present"));
    EXPECT_FALSE(store.exists("present"));
    EXPECT_FALSE(store.del("present"));
}

TEST(StoreTest, SupportsEmptyKeys) {
    Store store;
    store.set("", std::string{"empty"});

    const auto value = store.get("");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::get<std::string>(*value), "empty");
}

TEST(StoreTest, KeysStarReturnsAllKeysInStableOrder) {
    Store store{4};
    store.set("zulu", std::string{"z"});
    store.set("alpha", std::string{"a"});
    store.set("middle", std::string{"m"});

    const auto result = store.keys("*");

    EXPECT_EQ(result.status, KeysStatus::ok);
    EXPECT_EQ(result.keys, (std::vector<std::string>{"alpha", "middle", "zulu"}));
}

TEST(StoreTest, KeysExplicitlyRejectsUnsupportedPatterns) {
    Store store;
    store.set("alpha", Value{std::string{"a"}});

    const auto result = store.keys("a*");

    EXPECT_EQ(result.status, KeysStatus::unsupported_pattern);
    EXPECT_TRUE(result.keys.empty());
    EXPECT_TRUE(store.exists("alpha"));
}

} // namespace
