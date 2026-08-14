#pragma once

#include "novacache/commands/registry.hpp"
#include "novacache/net/reactor.hpp"
#include "novacache/net/socket.hpp"
#include "novacache/protocol/parser.hpp"
#include "novacache/server/stats.hpp"
#include "novacache/store/store.hpp"
#include "novacache/util/thread_pool.hpp"
#include "novacache/version.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace novacache::server {

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    int backlog = 128;
    std::size_t workers = 0;
    std::size_t shards = store::Store::default_shard_count;
    std::size_t max_connections = 10'000;
    std::size_t worker_queue_limit = 4'096;
    std::size_t max_output_buffer_bytes = 16U * 1024U * 1024U;
    int active_expiry_hz = 10;
    std::chrono::seconds idle_timeout{0};
    protocol::ParserLimits parser_limits{};
};

class Server {
  public:
    explicit Server(ServerConfig config = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void run();
    void stop() noexcept;

    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] Stats& stats() noexcept;
    [[nodiscard]] const Stats& stats() const noexcept;

  private:
    struct QueuedCommand {
        std::uint64_t sequence = 0;
        protocol::RespValue request;
    };

    struct Connection {
        std::uint64_t id = 0;
        net::Socket socket;
        net::Interest interest = net::Interest::read;
        std::string output;
        protocol::Parser parser;
        std::uint64_t next_request_seq = 0;
        std::uint64_t next_response_seq = 0;
        std::unordered_map<std::uint64_t, std::string> pending_responses;
        std::deque<QueuedCommand> command_queue;
        bool strand_running = false;
        std::size_t inflight = 0;
        bool read_closed = false;
        bool closing = false;
        std::chrono::steady_clock::time_point last_activity{};
    };

    struct Completion {
        std::uint64_t connection_id = 0;
        std::uint64_t sequence = 0;
        std::string payload;
        bool error = false;
    };

    void accept_ready();
    void handle_connection_event(int fd, net::Interest ready, bool error, bool hangup);
    void read_connection(Connection& connection);
    void write_connection(Connection& connection);
    void queue_parsed_commands(Connection& connection);
    void enqueue_command(Connection& connection, protocol::RespValue request);
    void launch_strand(Connection& connection);
    void drain_completions();
    void apply_completion(Completion completion);
    void update_interest(Connection& connection);
    void close_connection(int fd);
    void run_active_expiry();
    void close_idle_connections();
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] bool queue_saturated() const;

    ServerConfig config_;
    net::Socket listener_;
    std::uint16_t bound_port_ = 0;
    std::unique_ptr<net::Reactor> reactor_;
    store::Store store_;
    commands::Registry registry_;
    Stats stats_;
    std::unique_ptr<util::ThreadPool> workers_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::mutex completion_mutex_;
    std::deque<Completion> completions_;
    std::atomic_bool stopping_{false};
    std::uint64_t next_connection_id_ = 1;
    std::chrono::steady_clock::time_point next_expiry_tick_{};
};

} // namespace novacache::server
