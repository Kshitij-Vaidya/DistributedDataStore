#include "novacache/protocol/encoder.hpp"

#include <charconv>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

namespace novacache::protocol {
namespace {

template <typename... Callables> struct Overloaded : Callables... {
    using Callables::operator()...;
};

template <typename... Callables> Overloaded(Callables...) -> Overloaded<Callables...>;

void append_number(std::int64_t value, std::string& output) {
    char buffer[32]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("failed to encode RESP integer");
    }
    output.append(buffer, result.ptr);
}

void append_size(std::size_t value, std::string& output) {
    char buffer[32]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("failed to encode RESP length");
    }
    output.append(buffer, result.ptr);
}

void append_line(char prefix, std::string_view value, std::string& output) {
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        throw std::invalid_argument("RESP line values cannot contain CR or LF");
    }
    output.push_back(prefix);
    output.append(value);
    output.append("\r\n");
}

} // namespace

void append_encoded(const RespValue& value, std::string& output) {
    std::visit(
        Overloaded{
            [&output](const SimpleString& simple) { append_line('+', simple.value, output); },
            [&output](const Error& error) { append_line('-', error.value, output); },
            [&output](const Integer& integer) {
                output.push_back(':');
                append_number(integer.value, output);
                output.append("\r\n");
            },
            [&output](const BulkString& bulk) {
                if (!bulk.value.has_value()) {
                    output.append("$-1\r\n");
                    return;
                }
                output.push_back('$');
                append_size(bulk.value->size(), output);
                output.append("\r\n");
                output.append(*bulk.value);
                output.append("\r\n");
            },
            [&output](const Array& array) {
                if (!array.value.has_value()) {
                    output.append("*-1\r\n");
                    return;
                }
                output.push_back('*');
                append_size(array.value->size(), output);
                output.append("\r\n");
                for (const RespValue& item : *array.value) {
                    append_encoded(item, output);
                }
            },
        },
        value.storage());
}

std::string encode(const RespValue& value) {
    std::string output;
    append_encoded(value, output);
    return output;
}

} // namespace novacache::protocol
