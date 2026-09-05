#include "novacache/persistence/snapshot.hpp"

#include "novacache/persistence/binary.hpp"
#include "novacache/persistence/crc32.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

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
            throw std::runtime_error("short write to snapshot");
        }
        written += static_cast<std::size_t>(result);
    }
}

void fsync_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path{"."}
                                                   : path.parent_path();
    const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        return;
    }
    static_cast<void>(::fsync(dir_fd));
    static_cast<void>(::close(dir_fd));
}

void append_value(std::string& out, const store::Value& value) {
    std::visit(
        [&out](const auto& stored) {
            using Stored = std::decay_t<decltype(stored)>;
            if constexpr (std::is_same_v<Stored, std::string>) {
                append_u8(out, 0);
                append_bytes(out, stored);
            } else if constexpr (std::is_same_v<Stored, std::int64_t>) {
                append_u8(out, 1);
                append_i64(out, stored);
            } else if constexpr (std::is_same_v<Stored, store::StringList>) {
                append_u8(out, 2);
                append_u32(out, static_cast<std::uint32_t>(stored.size()));
                for (const std::string& item : stored) {
                    append_bytes(out, item);
                }
            } else if constexpr (std::is_same_v<Stored, store::StringSet>) {
                append_u8(out, 3);
                append_u32(out, static_cast<std::uint32_t>(stored.size()));
                for (const std::string& item : stored) {
                    append_bytes(out, item);
                }
            }
        },
        value);
}

[[nodiscard]] store::Value read_value(std::string_view& input) {
    const std::uint8_t type = read_u8(input);
    switch (type) {
    case 0:
        return read_bytes(input);
    case 1:
        return read_i64(input);
    case 2: {
        store::StringList list;
        const std::uint32_t count = read_u32(input);
        for (std::uint32_t index = 0; index < count; ++index) {
            list.push_back(read_bytes(input));
        }
        return list;
    }
    case 3: {
        store::StringSet set;
        const std::uint32_t count = read_u32(input);
        for (std::uint32_t index = 0; index < count; ++index) {
            set.insert(read_bytes(input));
        }
        return set;
    }
    default:
        throw std::runtime_error("unknown snapshot value type");
    }
}

} // namespace

std::int64_t to_unix_ms(const store::Clock::wall_time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

store::Clock::wall_time_point from_unix_ms(const std::int64_t millis) {
    return store::Clock::wall_time_point{std::chrono::milliseconds{millis}};
}

void SnapshotStore::write(const std::string_view path, const std::uint64_t wal_offset,
                          const std::vector<store::PersistedEntry>& entries) {
    namespace fs = std::filesystem;
    const fs::path final_path{std::string{path}};
    const fs::path temp_path = final_path.string() + ".tmp";
    fs::create_directories(final_path.parent_path());

    std::string body;
    body.append(magic);
    append_u32(body, version);
    append_u64(body, wal_offset);
    append_u64(body, static_cast<std::uint64_t>(entries.size()));
    for (const store::PersistedEntry& entry : entries) {
        append_bytes(body, entry.key);
        append_value(body, entry.value);
        append_u8(body, entry.expiry_wall.has_value() ? 1U : 0U);
        if (entry.expiry_wall.has_value()) {
            append_i64(body, to_unix_ms(*entry.expiry_wall));
        }
    }
    append_u32(body, crc32(std::string_view{body}));

    const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw_errno("open(snapshot tmp)");
    }
    try {
        write_all(fd, body);
        if (::fsync(fd) != 0) {
            throw_errno("fsync(snapshot tmp)");
        }
    } catch (...) {
        static_cast<void>(::close(fd));
        throw;
    }
    if (::close(fd) != 0) {
        throw_errno("close(snapshot tmp)");
    }

    std::error_code error;
    fs::rename(temp_path, final_path, error);
    if (error) {
        throw std::filesystem::filesystem_error("rename snapshot", temp_path, final_path, error);
    }
    fsync_parent_directory(final_path);
}

std::optional<std::pair<SnapshotHeader, std::vector<store::PersistedEntry>>>
SnapshotStore::read(const std::string_view path) {
    namespace fs = std::filesystem;
    const fs::path final_path{std::string{path}};
    if (!fs::exists(final_path)) {
        return std::nullopt;
    }

    const int fd = ::open(final_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::nullopt;
    }
    const off_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
        static_cast<void>(::close(fd));
        return std::nullopt;
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        static_cast<void>(::close(fd));
        return std::nullopt;
    }
    std::string file(static_cast<std::size_t>(size), '\0');
    std::size_t read_total = 0;
    while (read_total < file.size()) {
        const ssize_t got = ::read(fd, file.data() + read_total, file.size() - read_total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            static_cast<void>(::close(fd));
            return std::nullopt;
        }
        if (got == 0) {
            break;
        }
        read_total += static_cast<std::size_t>(got);
    }
    static_cast<void>(::close(fd));
    file.resize(read_total);

    if (file.size() < 8U + 4U + 8U + 8U + 4U || !file.starts_with(magic)) {
        return std::nullopt;
    }

    try {
        const std::string_view content{file.data(), file.size() - 4U};
        std::string_view trailer{file.data() + file.size() - 4U, 4U};
        const std::uint32_t expected = read_u32(trailer);
        if (crc32(content) != expected) {
            return std::nullopt;
        }

        std::string_view cursor = content;
        cursor.remove_prefix(8);
        if (read_u32(cursor) != version) {
            return std::nullopt;
        }
        SnapshotHeader header;
        header.wal_offset = read_u64(cursor);
        header.entry_count = read_u64(cursor);
        std::vector<store::PersistedEntry> entries;
        entries.reserve(static_cast<std::size_t>(header.entry_count));
        for (std::uint64_t index = 0; index < header.entry_count; ++index) {
            store::PersistedEntry entry;
            entry.key = read_bytes(cursor);
            entry.value = read_value(cursor);
            if (read_u8(cursor) != 0U) {
                entry.expiry_wall = from_unix_ms(read_i64(cursor));
            }
            entries.push_back(std::move(entry));
        }
        if (!cursor.empty()) {
            return std::nullopt;
        }
        return std::make_pair(header, std::move(entries));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace novacache::persistence
