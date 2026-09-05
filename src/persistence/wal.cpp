#include "novacache/persistence/wal.hpp"

#include "novacache/persistence/binary.hpp"
#include "novacache/persistence/crc32.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace novacache::persistence {
namespace {

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void write_all(const int fd, const std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("write");
        }
        if (result == 0) {
            throw std::runtime_error("short write to WAL");
        }
        written += static_cast<std::size_t>(result);
    }
}

[[nodiscard]] std::string encode_record_body(const std::uint64_t offset, const WalOpcode opcode,
                                             const std::string_view payload) {
    std::string body;
    append_u64(body, offset);
    append_u16(body, static_cast<std::uint16_t>(opcode));
    body.append(payload);
    return body;
}

} // namespace

WalWriter::WalWriter(std::string path) : path_(std::move(path)) {}

WalWriter::~WalWriter() {
    const std::lock_guard lock{mutex_};
    close_unlocked();
}

std::vector<WalRecord> WalWriter::open_and_recover() {
    const std::lock_guard lock{mutex_};
    namespace fs = std::filesystem;
    fs::create_directories(fs::path{path_}.parent_path());

    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw_errno("open(WAL)");
    }

    const off_t size = ::lseek(fd_, 0, SEEK_END);
    if (size < 0) {
        throw_errno("lseek(WAL end)");
    }

    std::vector<WalRecord> records;
    if (size == 0) {
        write_header_unlocked();
        last_offset_ = 0;
        return records;
    }

    if (size < 16) {
        if (::ftruncate(fd_, 0) != 0) {
            throw_errno("ftruncate(WAL short)");
        }
        write_header_unlocked();
        last_offset_ = 0;
        return records;
    }

    if (::lseek(fd_, 0, SEEK_SET) < 0) {
        throw_errno("lseek(WAL start)");
    }
    std::string file(static_cast<std::size_t>(size), '\0');
    std::size_t read_total = 0;
    while (read_total < file.size()) {
        const ssize_t got = ::read(fd_, file.data() + read_total, file.size() - read_total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("read(WAL)");
        }
        if (got == 0) {
            break;
        }
        read_total += static_cast<std::size_t>(got);
    }
    file.resize(read_total);

    if (!file.starts_with(magic) || file.size() < 16U) {
        if (::ftruncate(fd_, 0) != 0) {
            throw_errno("ftruncate(WAL bad magic)");
        }
        write_header_unlocked();
        last_offset_ = 0;
        return records;
    }

    std::string_view cursor{file};
    cursor.remove_prefix(8);
    const std::uint32_t file_version = read_u32(cursor);
    static_cast<void>(read_u32(cursor)); // reserved
    if (file_version != version) {
        throw std::runtime_error("unsupported WAL version");
    }

    std::size_t valid_end = 16;
    last_offset_ = 0;
    while (cursor.size() >= 4U) {
        const std::size_t record_start = valid_end;
        std::string_view probe = cursor;
        std::uint32_t length = 0;
        try {
            length = read_u32(probe);
        } catch (...) {
            break;
        }
        if (length < 10U || probe.size() < length + 4U) {
            break; // incomplete tail
        }
        const std::string_view body = probe.substr(0, length);
        probe.remove_prefix(length);
        const std::uint32_t expected_crc = read_u32(probe);
        const std::uint32_t actual_crc = crc32(body);
        if (expected_crc != actual_crc) {
            break; // corrupt tail
        }

        std::string_view body_view = body;
        WalRecord record;
        record.offset = read_u64(body_view);
        record.opcode = static_cast<WalOpcode>(read_u16(body_view));
        record.payload = std::string{body_view};
        if (record.offset <= last_offset_) {
            throw std::runtime_error("WAL offsets are not strictly increasing");
        }
        last_offset_ = record.offset;
        records.push_back(std::move(record));

        const std::size_t consumed = 4U + length + 4U;
        cursor.remove_prefix(consumed);
        valid_end = record_start + consumed;
    }

    if (static_cast<std::size_t>(size) != valid_end) {
        if (::ftruncate(fd_, static_cast<off_t>(valid_end)) != 0) {
            throw_errno("ftruncate(WAL tail)");
        }
        if (::fsync(fd_) != 0) {
            throw_errno("fsync(WAL truncate)");
        }
    }
    if (::lseek(fd_, 0, SEEK_END) < 0) {
        throw_errno("lseek(WAL resume)");
    }
    return records;
}

std::uint64_t WalWriter::append(const WalOpcode opcode, std::string payload) {
    const std::lock_guard lock{mutex_};
    ensure_open_unlocked();
    const std::uint64_t offset = last_offset_ + 1U;
    const std::string body = encode_record_body(offset, opcode, payload);
    std::string record;
    append_u32(record, static_cast<std::uint32_t>(body.size()));
    record.append(body);
    append_u32(record, crc32(body));
    write_all(fd_, record);
    last_offset_ = offset;
    return offset;
}

void WalWriter::sync() {
    const std::lock_guard lock{mutex_};
    ensure_open_unlocked();
    if (::fsync(fd_) != 0) {
        throw_errno("fsync(WAL)");
    }
}

void WalWriter::truncate_after(const std::uint64_t last_included_offset) {
    const std::lock_guard lock{mutex_};
    ensure_open_unlocked();
    if (::ftruncate(fd_, 0) != 0) {
        throw_errno("ftruncate(WAL rotate)");
    }
    write_header_unlocked();
    last_offset_ = last_included_offset;
    if (::fsync(fd_) != 0) {
        throw_errno("fsync(WAL rotate)");
    }
}

std::uint64_t WalWriter::last_offset() const {
    const std::lock_guard lock{mutex_};
    return last_offset_;
}

const std::string& WalWriter::path() const noexcept { return path_; }

void WalWriter::write_header_unlocked() {
    std::string header;
    header.append(magic.data(), 8);
    append_u32(header, version);
    append_u32(header, 0);
    if (::lseek(fd_, 0, SEEK_SET) < 0) {
        throw_errno("lseek(WAL header)");
    }
    write_all(fd_, header);
    if (::lseek(fd_, 0, SEEK_END) < 0) {
        throw_errno("lseek(WAL after header)");
    }
}

void WalWriter::ensure_open_unlocked() {
    if (fd_ < 0) {
        throw std::runtime_error("WAL is not open");
    }
}

void WalWriter::close_unlocked() noexcept {
    if (fd_ >= 0) {
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

std::string encode_set_payload(const std::string_view key, const std::string_view value,
                               const std::optional<std::int64_t> expiry_unix_ms) {
    std::string payload;
    append_bytes(payload, key);
    append_bytes(payload, value);
    append_u8(payload, expiry_unix_ms.has_value() ? 1U : 0U);
    if (expiry_unix_ms.has_value()) {
        append_i64(payload, *expiry_unix_ms);
    }
    return payload;
}

std::string encode_del_payload(const std::vector<std::string_view>& keys) {
    std::string payload;
    append_u32(payload, static_cast<std::uint32_t>(keys.size()));
    for (const std::string_view key : keys) {
        append_bytes(payload, key);
    }
    return payload;
}

std::string encode_expire_payload(const std::string_view key, const std::int64_t expiry_unix_ms) {
    std::string payload;
    append_bytes(payload, key);
    append_i64(payload, expiry_unix_ms);
    return payload;
}

} // namespace novacache::persistence
