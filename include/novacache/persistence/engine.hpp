#pragma once

#include "novacache/persistence/snapshot.hpp"
#include "novacache/persistence/wal.hpp"
#include "novacache/store/store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace novacache::persistence {

enum class FsyncMode {
    always,
    everysec,
    none,
};

struct PersistenceConfig {
    std::string data_dir;
    FsyncMode fsync = FsyncMode::everysec;
    int snapshot_interval_seconds = 300;
};

class PersistenceEngine {
  public:
    explicit PersistenceEngine(PersistenceConfig config,
                               std::shared_ptr<const store::Clock> clock =
                                   std::make_shared<store::SystemClock>());
    ~PersistenceEngine();

    PersistenceEngine(const PersistenceEngine&) = delete;
    PersistenceEngine& operator=(const PersistenceEngine&) = delete;

    void recover_into(store::Store& store);
    void shutdown(store::Store& store);
    // Closes without snapshotting — used to simulate a crash in tests.
    void abandon();

    void durable_set(store::Store& store, std::string key, std::string value);
    std::size_t durable_del(store::Store& store, const std::vector<std::string_view>& keys);
    bool durable_expire(store::Store& store, std::string_view key, std::chrono::seconds ttl);

    void maybe_snapshot(store::Store& store);
    void snapshot_now(store::Store& store);

    [[nodiscard]] std::uint64_t last_offset() const;
    [[nodiscard]] const PersistenceConfig& config() const noexcept;

  private:
    void start_fsync_thread();
    void stop_fsync_thread();
    void apply_record(store::Store& store, const WalRecord& record);
    void durability_action();

    PersistenceConfig config_;
    std::shared_ptr<const store::Clock> clock_;
    std::unique_ptr<WalWriter> wal_;
    mutable std::mutex mutex_;
    std::condition_variable fsync_cv_;
    std::thread fsync_thread_;
    std::atomic_bool stopping_{false};
    std::chrono::steady_clock::time_point next_snapshot_at_{};
};

} // namespace novacache::persistence
