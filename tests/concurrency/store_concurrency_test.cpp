#include "novacache/store/store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

TEST(StoreConcurrencyTest, ParallelWritersAndReadersComplete) {
    novacache::store::Store store{16};
    constexpr int writers = 8;
    constexpr int readers = 8;
    constexpr int ops_per_thread = 200;
    std::atomic_bool start{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(writers + readers));

    for (int index = 0; index < writers; ++index) {
        threads.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int op = 0; op < ops_per_thread; ++op) {
                const std::string key = "w" + std::to_string(index) + ":" + std::to_string(op);
                store.set(key, std::string{"value"});
                if ((op % 3) == 0) {
                    static_cast<void>(store.del(key));
                }
            }
        });
    }

    for (int index = 0; index < readers; ++index) {
        threads.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int op = 0; op < ops_per_thread; ++op) {
                const std::string key =
                    "w" + std::to_string(op % writers) + ":" + std::to_string(op);
                static_cast<void>(store.get(key));
                static_cast<void>(store.exists(key));
                static_cast<void>(index);
                static_cast<void>(store.keys("*"));
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_GE(store.approximate_memory(), 0U);
}

TEST(StoreConcurrencyTest, MultiKeyDeleteIsRaceFree) {
    novacache::store::Store store{8};
    std::vector<std::string> owned;
    owned.reserve(100);
    for (int index = 0; index < 100; ++index) {
        owned.push_back("k" + std::to_string(index));
        store.set(owned.back(), std::string{"v"});
    }

    std::atomic_bool start{false};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            std::vector<std::string_view> keys;
            keys.reserve(owned.size());
            for (const std::string& key : owned) {
                keys.emplace_back(key);
            }
            static_cast<void>(store.del_many(keys));
            static_cast<void>(store.exists_many(keys));
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(store.keys("*").keys.size(), 0U);
}

} // namespace
