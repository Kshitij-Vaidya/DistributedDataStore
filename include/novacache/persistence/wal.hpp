#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace novacache::persistence {

enum class WalOpcode : std::uint16_t {
    set = 1,
    del = 2,
    expire = 3,
};

struct WalRecord {
    std::uint64_t offset = 0;
    WalOpcode opcode = WalOpcode::set;
    std::string payload;
};

class WalWriter {
  public:
    static constexpr std::string_view magic{"NCWAL\0\0\0", 8};
    static constexpr std::uint32_t version = 1;

    explicit WalWriter(std::string path);
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // Opens or creates the file, validates/truncates a corrupt incomplete tail,
    // and returns intact records for recovery.
    [[nodiscard]] std::vector<WalRecord> open_and_recover();

    [[nodiscard]] std::uint64_t append(WalOpcode opcode, std::string payload);
    void sync();
    void truncate_after(std::uint64_t last_included_offset);

    [[nodiscard]] std::uint64_t last_offset() const;
    [[nodiscard]] const std::string& path() const noexcept;

  private:
    void write_header_unlocked();
    void ensure_open_unlocked();
    void close_unlocked() noexcept;

    std::string path_;
    int fd_ = -1;
    std::uint64_t last_offset_ = 0;
    mutable std::mutex mutex_;
};

[[nodiscard]] std::string encode_set_payload(std::string_view key, std::string_view value,
                                             std::optional<std::int64_t> expiry_unix_ms);
[[nodiscard]] std::string encode_del_payload(const std::vector<std::string_view>& keys);
[[nodiscard]] std::string encode_expire_payload(std::string_view key, std::int64_t expiry_unix_ms);

} // namespace novacache::persistence
