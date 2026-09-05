#include "novacache/net/socket.hpp"
#include "novacache/persistence/engine.hpp"
#include "novacache/protocol/encoder.hpp"
#include "novacache/protocol/parser.hpp"
#include "novacache/server/server.hpp"
#include "novacache/store/store.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

using novacache::protocol::RespValue;
using namespace std::chrono_literals;

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

    [[nodiscard]] RespValue receive() {
        std::array<char, 4096> buffer{};
        for (;;) {
            novacache::protocol::ParseResult result = parser_.next();
            if (result.status == novacache::protocol::ParseStatus::complete) {
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

  private:
    novacache::net::Socket socket_;
    novacache::protocol::Parser parser_;
};

class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("novacache-persist-" + std::to_string(::getpid()) + "-" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

TEST(PersistenceEngineTest, AlwaysModeSurvivesCrashWithoutSnapshot) {
    TempDir directory;
    novacache::store::Store store{4};

    {
        novacache::persistence::PersistenceEngine engine{
            novacache::persistence::PersistenceConfig{
                .data_dir = directory.path().string(),
                .fsync = novacache::persistence::FsyncMode::always,
                .snapshot_interval_seconds = 0,
            },
        };
        engine.recover_into(store);
        engine.durable_set(store, "demo", "value");
        engine.durable_expire(store, "demo", 3600s);
        engine.abandon();
    }

    novacache::store::Store restored{4};
    novacache::persistence::PersistenceEngine engine{
        novacache::persistence::PersistenceConfig{
            .data_dir = directory.path().string(),
            .fsync = novacache::persistence::FsyncMode::always,
            .snapshot_interval_seconds = 0,
        },
    };
    engine.recover_into(restored);
    const auto value = restored.get("demo");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::get<std::string>(*value), "value");
    EXPECT_GT(restored.ttl("demo"), 0);
}

TEST(PersistenceEngineTest, SnapshotThenWalReplayAndCorruptTail) {
    TempDir directory;
    novacache::store::Store store{4};
    novacache::persistence::PersistenceEngine engine{
        novacache::persistence::PersistenceConfig{
            .data_dir = directory.path().string(),
            .fsync = novacache::persistence::FsyncMode::always,
            .snapshot_interval_seconds = 0,
        },
    };
    engine.recover_into(store);
    engine.durable_set(store, "snap", "one");
    engine.snapshot_now(store);
    engine.durable_set(store, "wal", "two");
    engine.abandon();

    {
        std::ofstream out{(directory.path() / "append.ncwal").string(),
                          std::ios::binary | std::ios::app};
        out << "ZZ";
    }

    novacache::store::Store restored{4};
    novacache::persistence::PersistenceEngine recovery{
        novacache::persistence::PersistenceConfig{
            .data_dir = directory.path().string(),
            .fsync = novacache::persistence::FsyncMode::always,
            .snapshot_interval_seconds = 0,
        },
    };
    recovery.recover_into(restored);
    EXPECT_EQ(std::get<std::string>(*restored.get("snap")), "one");
    EXPECT_EQ(std::get<std::string>(*restored.get("wal")), "two");
}

TEST(PersistenceEngineTest, InvalidSnapshotFallsBackToWal) {
    TempDir directory;
    novacache::store::Store store{4};
    {
        novacache::persistence::PersistenceEngine engine{
            novacache::persistence::PersistenceConfig{
                .data_dir = directory.path().string(),
                .fsync = novacache::persistence::FsyncMode::always,
                .snapshot_interval_seconds = 0,
            },
        };
        engine.recover_into(store);
        engine.durable_set(store, "only-wal", "ok");
        engine.abandon();
    }

    {
        std::ofstream out{(directory.path() / "dump.ncs").string(),
                          std::ios::binary | std::ios::trunc};
        out << "not-a-snapshot";
    }

    novacache::store::Store restored{4};
    novacache::persistence::PersistenceEngine engine{
        novacache::persistence::PersistenceConfig{
            .data_dir = directory.path().string(),
            .fsync = novacache::persistence::FsyncMode::always,
            .snapshot_interval_seconds = 0,
        },
    };
    engine.recover_into(restored);
    EXPECT_EQ(std::get<std::string>(*restored.get("only-wal")), "ok");
}

TEST(ServerPersistenceTest, GracefulRestartPreservesData) {
    TempDir directory;
    std::uint16_t port = 0;

    {
        novacache::server::ServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.persistence_enabled = true;
        config.data_dir = directory.path().string();
        config.fsync = novacache::persistence::FsyncMode::always;
        config.snapshot_interval_seconds = 0;
        novacache::server::Server server{std::move(config)};
        port = server.bound_port();
        std::thread thread{[&server] { server.run(); }};
        Client client{port};
        client.send(command({"SET", "alpha", "persist-me"}));
        EXPECT_EQ(client.receive(), RespValue::simple("OK"));
        client.send(command({"EXPIRE", "alpha", "100"}));
        EXPECT_EQ(client.receive(), RespValue::integer(1));
        server.stop();
        thread.join();
    }

    novacache::server::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.persistence_enabled = true;
    config.data_dir = directory.path().string();
    config.fsync = novacache::persistence::FsyncMode::always;
    config.snapshot_interval_seconds = 0;
    novacache::server::Server server{std::move(config)};
    std::thread thread{[&server] { server.run(); }};
    Client client{server.bound_port()};
    client.send(command({"GET", "alpha"}));
    EXPECT_EQ(client.receive(), RespValue::bulk(std::string{"persist-me"}));
    client.send(command({"TTL", "alpha"}));
    const RespValue ttl = client.receive();
    const auto* const integer = std::get_if<novacache::protocol::Integer>(&ttl.storage());
    ASSERT_NE(integer, nullptr);
    EXPECT_GT(integer->value, 0);
    server.stop();
    thread.join();
}

} // namespace
