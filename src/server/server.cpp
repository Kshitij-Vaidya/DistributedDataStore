#include "novacache/server/server.hpp"

#include "novacache/protocol/encoder.hpp"

#include <array>
#include <exception>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace novacache::server {
namespace {

[[nodiscard]] bool is_command_request(const protocol::RespValue& request) {
    const auto* const array = std::get_if<protocol::Array>(&request.storage());
    if (array == nullptr || !array->value.has_value() || array->value->empty()) {
        return false;
    }
    for (const protocol::RespValue& item : *array->value) {
        const auto* const bulk = std::get_if<protocol::BulkString>(&item.storage());
        if (bulk == nullptr || !bulk->value.has_value()) {
            return false;
        }
    }
    return true;
}

void send_protocol_error(net::Socket& client) {
    client.send_all(protocol::encode(protocol::RespValue::error("ERR Protocol error")));
}

class ActiveSession {
  public:
    ActiveSession(std::mutex& mutex, net::Socket*& active, net::Socket& client)
        : mutex_(mutex), active_(active) {
        const std::lock_guard lock{mutex_};
        active_ = &client;
    }

    ~ActiveSession() {
        const std::lock_guard lock{mutex_};
        active_ = nullptr;
    }

    ActiveSession(const ActiveSession&) = delete;
    ActiveSession& operator=(const ActiveSession&) = delete;

  private:
    std::mutex& mutex_;
    net::Socket*& active_;
};

} // namespace

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      listener_(net::Socket::listen(config_.host, config_.port, config_.backlog)),
      bound_port_(listener_.local_port()) {}

Server::~Server() { stop(); }

void Server::run() {
    while (!stopping_.load(std::memory_order_acquire)) {
        net::Socket client;
        try {
            client = listener_.accept();
        } catch (const std::system_error&) {
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            throw;
        }
        if (stopping_.load(std::memory_order_acquire)) {
            return;
        }
        try {
            handle_session(client);
        } catch (const std::system_error&) {
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
        }
    }
}

void Server::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    {
        const std::lock_guard lock{active_mutex_};
        if (active_client_ != nullptr) {
            active_client_->shutdown();
        }
    }

    try {
        std::string wake_host = config_.host;
        if (wake_host.empty() || wake_host == "0.0.0.0") {
            wake_host = "127.0.0.1";
        } else if (wake_host == "::") {
            wake_host = "::1";
        }
        net::Socket wake = net::Socket::connect(wake_host, bound_port_);
        static_cast<void>(wake);
    } catch (const std::exception&) {
    }
    listener_.shutdown();
}

std::uint16_t Server::bound_port() const noexcept { return bound_port_; }

bool Server::stopping() const noexcept { return stopping_.load(std::memory_order_acquire); }

void Server::handle_session(net::Socket& client) {
    ActiveSession active{active_mutex_, active_client_, client};
    protocol::Parser parser{config_.parser_limits};
    std::array<char, 8192> buffer{};

    while (!stopping_.load(std::memory_order_acquire)) {
        const std::size_t received = client.receive(buffer);
        if (received == 0U) {
            if (parser.buffered_bytes() != 0U) {
                try {
                    send_protocol_error(client);
                } catch (const std::exception&) {
                }
            }
            return;
        }

        if (parser.feed(std::string_view{buffer.data(), received}) !=
            protocol::ParseStatus::incomplete) {
            send_protocol_error(client);
            return;
        }

        for (;;) {
            protocol::ParseResult result = parser.next();
            if (result.status == protocol::ParseStatus::incomplete) {
                break;
            }
            if (result.status != protocol::ParseStatus::complete || !result.value.has_value()) {
                send_protocol_error(client);
                return;
            }
            if (!is_command_request(*result.value)) {
                send_protocol_error(client);
                return;
            }

            const protocol::RespValue response = registry_.execute(*result.value, store_);
            client.send_all(protocol::encode(response));
        }
    }
}

} // namespace novacache::server
