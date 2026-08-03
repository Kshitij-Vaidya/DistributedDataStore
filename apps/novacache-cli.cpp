#include "novacache/net/socket.hpp"
#include "novacache/protocol/encoder.hpp"
#include "novacache/protocol/parser.hpp"
#include "novacache/version.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

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

void usage() {
    std::cerr << "usage: novacache-cli [-h host|--host host] [-p port|--port port] "
                 "command [argument ...]\n";
}

[[nodiscard]] novacache::protocol::RespValue
make_request(const std::vector<std::string>& arguments) {
    std::vector<novacache::protocol::RespValue> values;
    values.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        values.push_back(novacache::protocol::RespValue::bulk(argument));
    }
    return novacache::protocol::RespValue::array(std::move(values));
}

[[nodiscard]] bool render(const novacache::protocol::RespValue& value,
                          const std::size_t depth = 0) {
    if (const auto* const simple = std::get_if<novacache::protocol::SimpleString>(&value.storage());
        simple != nullptr) {
        std::cout << simple->value << '\n';
        return false;
    }
    if (const auto* const error = std::get_if<novacache::protocol::Error>(&value.storage());
        error != nullptr) {
        std::cerr << "(error) " << error->value << '\n';
        return true;
    }
    if (const auto* const integer = std::get_if<novacache::protocol::Integer>(&value.storage());
        integer != nullptr) {
        std::cout << integer->value << '\n';
        return false;
    }
    if (const auto* const bulk = std::get_if<novacache::protocol::BulkString>(&value.storage());
        bulk != nullptr) {
        if (bulk->value.has_value()) {
            std::cout << *bulk->value << '\n';
        } else {
            std::cout << "(nil)\n";
        }
        return false;
    }

    const auto& array = std::get<novacache::protocol::Array>(value.storage());
    if (!array.value.has_value()) {
        std::cout << "(nil)\n";
        return false;
    }
    if (array.value->empty()) {
        std::cout << "(empty array)\n";
        return false;
    }

    bool had_error = false;
    for (std::size_t index = 0; index < array.value->size(); ++index) {
        std::cout << std::string(depth * 2U, ' ') << index + 1U << ") ";
        had_error = render((*array.value)[index], depth + 1U) || had_error;
    }
    return had_error;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::vector<std::string> command;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--version" && argc == 2) {
            std::cout << "novacache-cli " << novacache::version() << '\n';
            return 0;
        }
        if ((argument == "-h" || argument == "--host") && command.empty()) {
            if (index + 1 >= argc) {
                usage();
                return 2;
            }
            host = argv[++index];
            continue;
        }
        if ((argument == "-p" || argument == "--port") && command.empty()) {
            if (index + 1 >= argc) {
                usage();
                return 2;
            }
            const std::optional<std::uint16_t> parsed = parse_port(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "novacache-cli: invalid port\n";
                return 2;
            }
            port = *parsed;
            continue;
        }
        command.emplace_back(argument);
    }

    if (command.empty()) {
        usage();
        return 2;
    }

    try {
        novacache::net::Socket socket = novacache::net::Socket::connect(host, port);
        socket.send_all(novacache::protocol::encode(make_request(command)));

        novacache::protocol::Parser parser;
        std::array<char, 8192> buffer{};
        for (;;) {
            novacache::protocol::ParseResult result = parser.next();
            if (result.status == novacache::protocol::ParseStatus::complete &&
                result.value.has_value()) {
                return render(*result.value) ? 1 : 0;
            }
            if (result.status != novacache::protocol::ParseStatus::incomplete) {
                std::cerr << "novacache-cli: malformed server response\n";
                return 1;
            }

            const std::size_t received = socket.receive(buffer);
            if (received == 0U) {
                std::cerr << "novacache-cli: server closed the connection\n";
                return 1;
            }
            if (parser.feed(std::string_view{buffer.data(), received}) !=
                novacache::protocol::ParseStatus::incomplete) {
                std::cerr << "novacache-cli: server response exceeded limits\n";
                return 1;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "novacache-cli: " << error.what() << '\n';
        return 1;
    }
}
