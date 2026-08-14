#include "novacache/server/stats.hpp"

#include <sstream>

namespace novacache::server {

void Stats::on_accept() noexcept {
    accepted_connections_.fetch_add(1, std::memory_order_relaxed);
    active_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::on_disconnect() noexcept {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
}

void Stats::on_command(const bool error) noexcept {
    total_commands_.fetch_add(1, std::memory_order_relaxed);
    if (error) {
        command_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Stats::add_bytes_in(const std::uint64_t bytes) noexcept {
    bytes_in_.fetch_add(bytes, std::memory_order_relaxed);
}

void Stats::add_bytes_out(const std::uint64_t bytes) noexcept {
    bytes_out_.fetch_add(bytes, std::memory_order_relaxed);
}

void Stats::add_expired(const std::uint64_t count) noexcept {
    expired_keys_.fetch_add(count, std::memory_order_relaxed);
}

void Stats::add_evicted(const std::uint64_t count) noexcept {
    evicted_keys_.fetch_add(count, std::memory_order_relaxed);
}

void Stats::set_used_memory(const std::uint64_t bytes) noexcept {
    used_memory_.store(bytes, std::memory_order_relaxed);
}

std::uint64_t Stats::accepted_connections() const noexcept {
    return accepted_connections_.load(std::memory_order_relaxed);
}

std::uint64_t Stats::active_connections() const noexcept {
    return active_connections_.load(std::memory_order_relaxed);
}

std::uint64_t Stats::total_commands() const noexcept {
    return total_commands_.load(std::memory_order_relaxed);
}

std::uint64_t Stats::command_errors() const noexcept {
    return command_errors_.load(std::memory_order_relaxed);
}

std::uint64_t Stats::bytes_in() const noexcept { return bytes_in_.load(std::memory_order_relaxed); }

std::uint64_t Stats::bytes_out() const noexcept {
    return bytes_out_.load(std::memory_order_relaxed);
}

std::uint64_t Stats::expired_keys() const noexcept {
    return expired_keys_.load(std::memory_order_relaxed);
}

std::uint64_t Stats::evicted_keys() const noexcept {
    return evicted_keys_.load(std::memory_order_relaxed);
}

std::uint64_t Stats::used_memory() const noexcept {
    return used_memory_.load(std::memory_order_relaxed);
}

std::string Stats::render_info(const std::string_view version, const std::uint16_t tcp_port,
                               const std::size_t worker_count,
                               const std::size_t shard_count) const {
    std::ostringstream output;
    output << "# Server\r\n"
           << "novacache_version:" << version << "\r\n"
           << "tcp_port:" << tcp_port << "\r\n"
           << "io_threads:1\r\n"
           << "worker_threads:" << worker_count << "\r\n"
           << "shard_count:" << shard_count << "\r\n"
           << "\r\n"
           << "# Clients\r\n"
           << "connected_clients:" << active_connections() << "\r\n"
           << "total_connections_received:" << accepted_connections() << "\r\n"
           << "\r\n"
           << "# Stats\r\n"
           << "total_commands_processed:" << total_commands() << "\r\n"
           << "total_error_replies:" << command_errors() << "\r\n"
           << "total_net_input_bytes:" << bytes_in() << "\r\n"
           << "total_net_output_bytes:" << bytes_out() << "\r\n"
           << "expired_keys:" << expired_keys() << "\r\n"
           << "evicted_keys:" << evicted_keys() << "\r\n"
           << "\r\n"
           << "# Memory\r\n"
           << "used_memory:" << used_memory() << "\r\n";
    return output.str();
}

} // namespace novacache::server
