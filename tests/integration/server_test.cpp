#include "novacache/net/socket.hpp"
#include "novacache/protocol/encoder.hpp"
#include "novacache/protocol/parser.hpp"
#include "novacache/server/server.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using novacache::protocol::RespValue;

[[nodiscard]] RespValue command(const std::initializer_list<std::string_view> arguments) {
    std::vector<RespValue> items;
    items.reserve(arguments.size());
    for (const std::string_view argument : arguments) {
        items.push_back(RespValue::bulk(std::string{argument}));
    }
    return RespValue::array(std::move(items));
}

class Client {
  public:
    explicit Client(const std::uint16_t port)
        : socket_(novacache::net::Socket::connect("127.0.0.1", port)) {}

    void send(const RespValue& request) { socket_.send_all(novacache::protocol::encode(request)); }

    void send_bytes(const std::string_view bytes) { socket_.send_all(bytes); }

    [[nodiscard]] RespValue receive() {
        std::array<char, 4096> buffer{};
        for (;;) {
            novacache::protocol::ParseResult result = parser_.next();
            if (result.status == novacache::protocol::ParseStatus::complete) {
                if (!result.value.has_value()) {
                    throw std::runtime_error("complete response has no value");
                }
                return *result.value;
            }
            if (result.status != novacache::protocol::ParseStatus::incomplete) {
                throw std::runtime_error("malformed response");
            }
            const std::size_t received = socket_.receive(buffer);
            if (received == 0U) {
                throw std::runtime_error("unexpected response EOF");
            }
            static_cast<void>(parser_.feed(std::string_view{buffer.data(), received}));
        }
    }

    [[nodiscard]] std::size_t receive_raw(std::span<char> buffer) {
        return socket_.receive(buffer);
    }

  private:
    novacache::net::Socket socket_;
    novacache::protocol::Parser parser_;
};

class ServerTest : public testing::Test {
  protected:
    ServerTest()
        : server_(novacache::server::ServerConfig{.host = "127.0.0.1", .port = 0}), thread_([this] {
              try {
                  server_.run();
              } catch (...) {
                  server_error_ = std::current_exception();
              }
          }) {}

    void TearDown() override {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        EXPECT_EQ(server_error_, nullptr);
    }

    novacache::server::Server server_;
    std::exception_ptr server_error_;
    std::thread thread_;
};

TEST(CommandRegistryTest, PublishesMetadataAndRejectsGetOfNonStringValues) {
    novacache::commands::Registry registry;
    const novacache::commands::CommandMetadata* const metadata = registry.metadata("eXiStS");
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->name, "EXISTS");
    EXPECT_EQ(metadata->minimum_arity, 2U);
    EXPECT_EQ(metadata->access, novacache::commands::CommandAccess::read_only);
    EXPECT_FALSE(metadata->persistent);
    EXPECT_FALSE(metadata->replicated);

    const novacache::commands::CommandMetadata* const set_metadata = registry.metadata("SET");
    ASSERT_NE(set_metadata, nullptr);
    EXPECT_EQ(set_metadata->access, novacache::commands::CommandAccess::write);
    EXPECT_TRUE(set_metadata->persistent);
    EXPECT_TRUE(set_metadata->replicated);

    novacache::store::Store store;
    store.set("integer", std::int64_t{42});
    EXPECT_EQ(
        registry.execute(command({"GET", "integer"}), store),
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value"));
}

TEST_F(ServerTest, ExecutesTheInitialCommandSet) {
    Client client{server_.bound_port()};

    client.send(command({"PING"}));
    EXPECT_EQ(client.receive(), RespValue::simple("PONG"));
    client.send(command({"pInG", "hello"}));
    EXPECT_EQ(client.receive(), RespValue::bulk(std::string{"hello"}));
    client.send(command({"ECHO", "echoed"}));
    EXPECT_EQ(client.receive(), RespValue::bulk(std::string{"echoed"}));

    client.send(command({"SET", "alpha", "one"}));
    EXPECT_EQ(client.receive(), RespValue::simple("OK"));
    client.send(command({"GET", "alpha"}));
    EXPECT_EQ(client.receive(), RespValue::bulk(std::string{"one"}));
    client.send(command({"GET", "missing"}));
    EXPECT_EQ(client.receive(), RespValue::bulk(std::nullopt));

    client.send(command({"EXISTS", "alpha", "alpha", "missing"}));
    EXPECT_EQ(client.receive(), RespValue::integer(2));
    client.send(command({"KEYS", "*"}));
    EXPECT_EQ(client.receive(),
              RespValue::array(std::vector<RespValue>{RespValue::bulk(std::string{"alpha"})}));

    client.send(command({"EXPIRE", "missing", "10"}));
    EXPECT_EQ(client.receive(), RespValue::integer(0));
    client.send(command({"EXPIRE", "alpha", "10"}));
    EXPECT_EQ(client.receive(), RespValue::integer(1));
    client.send(command({"TTL", "alpha"}));
    const RespValue ttl_response = client.receive();
    const auto* const ttl = std::get_if<novacache::protocol::Integer>(&ttl_response.storage());
    ASSERT_NE(ttl, nullptr);
    EXPECT_GE(ttl->value, 9);
    EXPECT_LE(ttl->value, 10);

    client.send(command({"DEL", "alpha", "alpha", "missing"}));
    EXPECT_EQ(client.receive(), RespValue::integer(1));
    client.send(command({"TTL", "alpha"}));
    EXPECT_EQ(client.receive(), RespValue::integer(-2));
}

TEST_F(ServerTest, ReturnsDocumentedCommandErrors) {
    Client client{server_.bound_port()};

    client.send(command({"PING", "one", "two"}));
    EXPECT_EQ(client.receive(), RespValue::error("ERR wrong number of arguments"));
    client.send(command({"NOPE"}));
    EXPECT_EQ(client.receive(), RespValue::error("ERR unknown command"));
    client.send(command({"EXPIRE", "key", "9223372036854775808"}));
    EXPECT_EQ(client.receive(), RespValue::error("ERR value is not an integer or out of range"));
    client.send(command({"KEYS", "a*"}));
    EXPECT_EQ(client.receive(), RespValue::error("ERR unsupported pattern"));
}

TEST_F(ServerTest, HandlesFragmentedRequestsAndPipelining) {
    Client client{server_.bound_port()};
    const std::string fragmented = novacache::protocol::encode(command({"ECHO", "pieces"}));
    for (const char byte : fragmented) {
        client.send_bytes(std::string_view{&byte, 1});
    }
    EXPECT_EQ(client.receive(), RespValue::bulk(std::string{"pieces"}));

    const std::string pipeline = novacache::protocol::encode(command({"SET", "key", "value"})) +
                                 novacache::protocol::encode(command({"GET", "key"})) +
                                 novacache::protocol::encode(command({"DEL", "key"}));
    client.send_bytes(pipeline);
    EXPECT_EQ(client.receive(), RespValue::simple("OK"));
    EXPECT_EQ(client.receive(), RespValue::bulk(std::string{"value"}));
    EXPECT_EQ(client.receive(), RespValue::integer(1));
}

TEST_F(ServerTest, ReportsMalformedProtocolThenClosesSession) {
    {
        Client client{server_.bound_port()};
        client.send_bytes("!\r\n");
        EXPECT_EQ(client.receive(), RespValue::error("ERR Protocol error"));

        std::array<char, 1> byte{};
        EXPECT_EQ(client.receive_raw(byte), 0U);
    }

    Client invalid_request{server_.bound_port()};
    invalid_request.send_bytes("*-1\r\n");
    EXPECT_EQ(invalid_request.receive(), RespValue::error("ERR Protocol error"));
    std::array<char, 1> byte{};
    EXPECT_EQ(invalid_request.receive_raw(byte), 0U);
}

TEST_F(ServerTest, StopsWhileSessionReceiveIsBlocked) {
    Client client{server_.bound_port()};
    client.send(command({"PING"}));
    EXPECT_EQ(client.receive(), RespValue::simple("PONG"));

    server_.stop();
    thread_.join();
    EXPECT_EQ(server_error_, nullptr);
}

TEST_F(ServerTest, ServesConcurrentClientsAndPipelines) {
    constexpr int client_count = 32;
    constexpr int requests_per_client = 50;
    std::vector<std::thread> clients;
    std::atomic_int failures{0};
    std::atomic_bool start{false};

    for (int index = 0; index < client_count; ++index) {
        clients.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
            }
            try {
                Client client{server_.bound_port()};
                std::string pipeline;
                for (int request = 0; request < requests_per_client; ++request) {
                    const std::string key =
                        "c" + std::to_string(index) + ":" + std::to_string(request);
                    pipeline += novacache::protocol::encode(command({"SET", key, "v"}));
                    pipeline += novacache::protocol::encode(command({"GET", key}));
                }
                client.send_bytes(pipeline);
                for (int request = 0; request < requests_per_client; ++request) {
                    if (client.receive() != RespValue::simple("OK")) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (client.receive() != RespValue::bulk(std::string{"v"})) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& client : clients) {
        client.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GE(server_.stats().total_commands(),
              static_cast<std::uint64_t>(client_count * requests_per_client * 2));
}

TEST_F(ServerTest, InfoReportsStableStatsText) {
    Client client{server_.bound_port()};
    client.send(command({"PING"}));
    EXPECT_EQ(client.receive(), RespValue::simple("PONG"));
    client.send(command({"INFO"}));
    const RespValue info = client.receive();
    const auto* const bulk = std::get_if<novacache::protocol::BulkString>(&info.storage());
    ASSERT_NE(bulk, nullptr);
    ASSERT_TRUE(bulk->value.has_value());
    EXPECT_NE(bulk->value->find("novacache_version:"), std::string::npos);
    EXPECT_NE(bulk->value->find("total_commands_processed:"), std::string::npos);
    EXPECT_NE(bulk->value->find("connected_clients:"), std::string::npos);
    EXPECT_NE(bulk->value->find("worker_threads:"), std::string::npos);
}

} // namespace
