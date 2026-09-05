#include "novacache/persistence/engine.hpp"

#include "novacache/persistence/binary.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace novacache::persistence {
namespace {

[[nodiscard]] std::string wal_path(const std::string& data_dir) {
    return (std::filesystem::path{data_dir} / "append.ncwal").string();
}

[[nodiscard]] std::string snapshot_path(const std::string& data_dir) {
    return (std::filesystem::path{data_dir} / "dump.ncs").string();
}

} // namespace

PersistenceEngine::PersistenceEngine(PersistenceConfig config,
                                     std::shared_ptr<const store::Clock> clock)
    : config_(std::move(config)), clock_(std::move(clock)) {
    if (config_.data_dir.empty()) {
        throw std::invalid_argument("persistence data_dir must not be empty");
    }
    if (clock_ == nullptr) {
        throw std::invalid_argument("persistence clock must not be null");
    }
    std::filesystem::create_directories(config_.data_dir);
    wal_ = std::make_unique<WalWriter>(wal_path(config_.data_dir));
    next_snapshot_at_ = std::chrono::steady_clock::now();
    if (config_.snapshot_interval_seconds > 0) {
        next_snapshot_at_ += std::chrono::seconds{config_.snapshot_interval_seconds};
    }
}

PersistenceEngine::~PersistenceEngine() { stop_fsync_thread(); }

void PersistenceEngine::recover_into(store::Store& store) {
    const std::lock_guard lock{mutex_};
    store.clear();

    auto loaded = SnapshotStore::read(snapshot_path(config_.data_dir));
    std::uint64_t snapshot_offset = 0;
    if (loaded.has_value()) {
        snapshot_offset = loaded->first.wal_offset;
        for (store::PersistedEntry& entry : loaded->second) {
            store.import_snapshot_entry(std::move(entry));
        }
    }

    std::vector<WalRecord> records = wal_->open_and_recover();
    for (const WalRecord& record : records) {
        if (record.offset <= snapshot_offset) {
            continue;
        }
        apply_record(store, record);
    }

    if (config_.fsync == FsyncMode::everysec) {
        start_fsync_thread();
    }
}

void PersistenceEngine::shutdown(store::Store& store) {
    stop_fsync_thread();
    const std::lock_guard lock{mutex_};
    if (wal_ == nullptr) {
        return;
    }
    if (config_.fsync != FsyncMode::none) {
        wal_->sync();
    }
    const auto entries = store.export_snapshot_entries();
    const std::uint64_t offset = wal_->last_offset();
    SnapshotStore::write(snapshot_path(config_.data_dir), offset, entries);
    wal_->truncate_after(offset);
}

void PersistenceEngine::abandon() {
    stop_fsync_thread();
    const std::lock_guard lock{mutex_};
    if (wal_ != nullptr && config_.fsync == FsyncMode::always) {
        try {
            wal_->sync();
        } catch (...) {
        }
    }
    wal_.reset();
}

void PersistenceEngine::durable_set(store::Store& store, std::string key, std::string value) {
    const std::lock_guard lock{mutex_};
    const auto payload = encode_set_payload(key, value, std::nullopt);
    static_cast<void>(wal_->append(WalOpcode::set, payload));
    durability_action();
    store.set(std::move(key), std::move(value));
}

std::size_t PersistenceEngine::durable_del(store::Store& store,
                                           const std::vector<std::string_view>& keys) {
    const std::lock_guard lock{mutex_};
    const auto payload = encode_del_payload(keys);
    static_cast<void>(wal_->append(WalOpcode::del, payload));
    durability_action();
    return store.del_many(keys);
}

bool PersistenceEngine::durable_expire(store::Store& store, const std::string_view key,
                                       const std::chrono::seconds ttl) {
    const std::lock_guard lock{mutex_};
    std::int64_t expiry_ms = 0;
    if (ttl > std::chrono::seconds::zero()) {
        expiry_ms = to_unix_ms(clock_->wall_now() + ttl);
    } else {
        expiry_ms = to_unix_ms(clock_->wall_now());
    }
    const auto payload = encode_expire_payload(key, expiry_ms);
    static_cast<void>(wal_->append(WalOpcode::expire, payload));
    durability_action();
    return store.expire_at(key, from_unix_ms(expiry_ms));
}

void PersistenceEngine::maybe_snapshot(store::Store& store) {
    if (config_.snapshot_interval_seconds <= 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_snapshot_at_) {
        return;
    }
    snapshot_now(store);
}

void PersistenceEngine::snapshot_now(store::Store& store) {
    const std::lock_guard lock{mutex_};
    if (config_.fsync != FsyncMode::none) {
        wal_->sync();
    }
    const auto entries = store.export_snapshot_entries();
    const std::uint64_t offset = wal_->last_offset();
    SnapshotStore::write(snapshot_path(config_.data_dir), offset, entries);
    wal_->truncate_after(offset);
    if (config_.snapshot_interval_seconds > 0) {
        next_snapshot_at_ = std::chrono::steady_clock::now() +
                            std::chrono::seconds{config_.snapshot_interval_seconds};
    }
}

std::uint64_t PersistenceEngine::last_offset() const {
    const std::lock_guard lock{mutex_};
    return wal_ == nullptr ? 0 : wal_->last_offset();
}

const PersistenceConfig& PersistenceEngine::config() const noexcept { return config_; }

void PersistenceEngine::start_fsync_thread() {
    if (fsync_thread_.joinable()) {
        return;
    }
    stopping_.store(false, std::memory_order_release);
    fsync_thread_ = std::thread([this] {
        while (!stopping_.load(std::memory_order_acquire)) {
            {
                std::unique_lock lock{mutex_};
                fsync_cv_.wait_for(lock, std::chrono::seconds{1},
                                   [this] { return stopping_.load(std::memory_order_acquire); });
                if (stopping_.load(std::memory_order_acquire)) {
                    break;
                }
            }
            try {
                wal_->sync();
            } catch (...) {
            }
        }
    });
}

void PersistenceEngine::stop_fsync_thread() {
    stopping_.store(true, std::memory_order_release);
    fsync_cv_.notify_all();
    if (fsync_thread_.joinable()) {
        fsync_thread_.join();
    }
}

void PersistenceEngine::apply_record(store::Store& store, const WalRecord& record) {
    std::string_view payload{record.payload};
    switch (record.opcode) {
    case WalOpcode::set: {
        std::string key = read_bytes(payload);
        std::string value = read_bytes(payload);
        std::optional<store::Clock::wall_time_point> expiry;
        if (read_u8(payload) != 0U) {
            expiry = from_unix_ms(read_i64(payload));
        }
        store.set(std::move(key), std::move(value), expiry);
        break;
    }
    case WalOpcode::del: {
        const std::uint32_t count = read_u32(payload);
        std::vector<std::string> owned;
        owned.reserve(count);
        std::vector<std::string_view> keys;
        keys.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            owned.push_back(read_bytes(payload));
            keys.emplace_back(owned.back());
        }
        static_cast<void>(store.del_many(keys));
        break;
    }
    case WalOpcode::expire: {
        const std::string key = read_bytes(payload);
        const auto expiry = from_unix_ms(read_i64(payload));
        static_cast<void>(store.expire_at(key, expiry));
        break;
    }
    default:
        throw std::runtime_error("unknown WAL opcode during recovery");
    }
}

void PersistenceEngine::durability_action() {
    if (config_.fsync == FsyncMode::always) {
        wal_->sync();
    }
}

} // namespace novacache::persistence
