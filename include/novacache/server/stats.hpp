#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace novacache::server {

class Stats {
  public:
    void on_accept() noexcept;
    void on_disconnect() noexcept;
    void on_command(bool error) noexcept;
    void add_bytes_in(std::uint64_t bytes) noexcept;
    void add_bytes_out(std::uint64_t bytes) noexcept;
    void add_expired(std::uint64_t count) noexcept;
    void add_evicted(std::uint64_t count) noexcept;
    void set_used_memory(std::uint64_t bytes) noexcept;

    [[nodiscard]] std::uint64_t accepted_connections() const noexcept;
    [[nodiscard]] std::uint64_t active_connections() const noexcept;
    [[nodiscard]] std::uint64_t total_commands() const noexcept;
    [[nodiscard]] std::uint64_t command_errors() const noexcept;
    [[nodiscard]] std::uint64_t bytes_in() const noexcept;
    [[nodiscard]] std::uint64_t bytes_out() const noexcept;
    [[nodiscard]] std::uint64_t expired_keys() const noexcept;
    [[nodiscard]] std::uint64_t evicted_keys() const noexcept;
    [[nodiscard]] std::uint64_t used_memory() const noexcept;

    [[nodiscard]] std::string render_info(std::string_view version, std::uint16_t tcp_port,
                                          std::size_t worker_count, std::size_t shard_count) const;

  private:
    std::atomic<std::uint64_t> accepted_connections_{0};
    std::atomic<std::uint64_t> active_connections_{0};
    std::atomic<std::uint64_t> total_commands_{0};
    std::atomic<std::uint64_t> command_errors_{0};
    std::atomic<std::uint64_t> bytes_in_{0};
    std::atomic<std::uint64_t> bytes_out_{0};
    std::atomic<std::uint64_t> expired_keys_{0};
    std::atomic<std::uint64_t> evicted_keys_{0};
    std::atomic<std::uint64_t> used_memory_{0};
};

} // namespace novacache::server
