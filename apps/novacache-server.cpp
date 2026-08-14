#include "novacache/server/server.hpp"
#include "novacache/version.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

[[nodiscard]] std::optional<std::uint16_t> parse_port(const std::string_view text) {
    unsigned int value = 0;
    const char* const begin = text.data();
    const auto result = std::from_chars(begin, begin + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != begin + text.size() ||
        value > 65535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::optional<std::size_t> parse_size(const std::string_view text) {
    std::size_t value = 0;
    const char* const begin = text.data();
    const auto result = std::from_chars(begin, begin + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != begin + text.size()) {
        return std::nullopt;
    }
    return value;
}

void usage() {
    std::cerr << "usage: novacache-server [-h host|--host host] [-p port|--port port] "
                 "[--workers N]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    novacache::server::ServerConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--version") {
            std::cout << "novacache-server " << novacache::version() << '\n';
            return 0;
        }
        if (argument == "-h" || argument == "--host") {
            if (index + 1 >= argc) {
                usage();
                return 2;
            }
            config.host = argv[++index];
            continue;
        }
        if (argument == "-p" || argument == "--port") {
            if (index + 1 >= argc) {
                usage();
                return 2;
            }
            const std::optional<std::uint16_t> port = parse_port(argv[++index]);
            if (!port.has_value()) {
                std::cerr << "novacache-server: invalid port\n";
                return 2;
            }
            config.port = *port;
            continue;
        }
        if (argument == "--workers") {
            if (index + 1 >= argc) {
                usage();
                return 2;
            }
            const std::optional<std::size_t> workers = parse_size(argv[++index]);
            if (!workers.has_value()) {
                std::cerr << "novacache-server: invalid workers\n";
                return 2;
            }
            config.workers = *workers;
            continue;
        }
        usage();
        return 2;
    }

    try {
        novacache::server::Server server{std::move(config)};
        std::cout << "NovaCache listening on port " << server.bound_port() << std::endl;
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "novacache-server: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
