#pragma once

#include "novacache/commands/registry.hpp"
#include "novacache/net/socket.hpp"
#include "novacache/protocol/parser.hpp"
#include "novacache/store/store.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace novacache::server {

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    int backlog = 128;
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

  private:
    void handle_session(net::Socket& client);

    ServerConfig config_;
    net::Socket listener_;
    std::uint16_t bound_port_ = 0;
    store::Store store_;
    commands::Registry registry_;
    std::atomic_bool stopping_{false};
    std::mutex active_mutex_;
    net::Socket* active_client_ = nullptr;
};

} // namespace novacache::server
