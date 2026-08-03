#include "novacache/protocol/parser.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace novacache::protocol {
namespace {

struct LineResult {
    ParseStatus status = ParseStatus::incomplete;
    std::string_view value;
    std::size_t next = 0;
    const char* message = "";
};

struct ValueResult {
    ParseStatus status = ParseStatus::incomplete;
    std::optional<RespValue> value;
    std::size_t consumed = 0;
    const char* message = "";
};

[[nodiscard]] LineResult read_line(std::string_view input, std::size_t start) {
    for (std::size_t index = start; index < input.size(); ++index) {
        if (input[index] == '\n') {
            return {ParseStatus::malformed, {}, 0, "line feed without preceding carriage return"};
        }
        if (input[index] != '\r') {
            continue;
        }
        if (index + 1U == input.size()) {
            return {};
        }
        if (input[index + 1U] != '\n') {
            return {ParseStatus::malformed, {}, 0, "carriage return not followed by line feed"};
        }
        return {ParseStatus::complete, input.substr(start, index - start), index + 2U, ""};
    }
    return {};
}

[[nodiscard]] bool parse_integer(std::string_view text, std::int64_t& value) {
    if (text.empty() || text.front() == '+') {
        return false;
    }

    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] ValueResult parse_value(std::string_view input, std::size_t depth,
                                      const ParserLimits& limits);

[[nodiscard]] ValueResult parse_line_value(std::string_view input, bool is_error) {
    const LineResult line = read_line(input, 1U);
    if (line.status != ParseStatus::complete) {
        return {line.status, std::nullopt, 0, line.message};
    }

    std::string value{line.value};
    if (is_error) {
        return {ParseStatus::complete, RespValue::error(std::move(value)), line.next, ""};
    }
    return {ParseStatus::complete, RespValue::simple(std::move(value)), line.next, ""};
}

[[nodiscard]] ValueResult parse_resp_integer(std::string_view input) {
    const LineResult line = read_line(input, 1U);
    if (line.status != ParseStatus::complete) {
        return {line.status, std::nullopt, 0, line.message};
    }

    std::int64_t value = 0;
    if (!parse_integer(line.value, value)) {
        return {ParseStatus::malformed, std::nullopt, 0, "invalid or overflowing integer"};
    }
    return {ParseStatus::complete, RespValue::integer(value), line.next, ""};
}

[[nodiscard]] ValueResult parse_bulk(std::string_view input, const ParserLimits& limits) {
    const LineResult line = read_line(input, 1U);
    if (line.status != ParseStatus::complete) {
        return {line.status, std::nullopt, 0, line.message};
    }

    std::int64_t length = 0;
    if (!parse_integer(line.value, length)) {
        return {ParseStatus::malformed, std::nullopt, 0, "invalid or overflowing bulk length"};
    }
    if (length == -1) {
        return {ParseStatus::complete, RespValue::bulk(std::nullopt), line.next, ""};
    }
    if (length < 0) {
        return {ParseStatus::malformed, std::nullopt, 0, "illegal negative bulk length"};
    }

    const auto unsigned_length = static_cast<std::uint64_t>(length);
    if (unsigned_length > limits.max_bulk_size) {
        return {ParseStatus::oversized, std::nullopt, 0, "bulk string exceeds configured limit"};
    }
    if (unsigned_length > std::numeric_limits<std::size_t>::max()) {
        return {ParseStatus::oversized, std::nullopt, 0, "bulk string length is not representable"};
    }

    const auto size = static_cast<std::size_t>(unsigned_length);
    if (size > input.size() - line.next) {
        return {};
    }
    const std::size_t payload_end = line.next + size;
    if (payload_end == input.size()) {
        return {};
    }
    if (input[payload_end] != '\r') {
        return {ParseStatus::malformed, std::nullopt, 0, "bulk payload lacks terminating CRLF"};
    }
    if (payload_end + 1U == input.size()) {
        return {};
    }
    if (input[payload_end + 1U] != '\n') {
        return {ParseStatus::malformed, std::nullopt, 0, "bulk payload lacks terminating CRLF"};
    }

    std::string value{input.substr(line.next, size)};
    return {ParseStatus::complete, RespValue::bulk(std::move(value)), payload_end + 2U, ""};
}

[[nodiscard]] ValueResult parse_array(std::string_view input, std::size_t depth,
                                      const ParserLimits& limits) {
    const LineResult line = read_line(input, 1U);
    if (line.status != ParseStatus::complete) {
        return {line.status, std::nullopt, 0, line.message};
    }

    std::int64_t length = 0;
    if (!parse_integer(line.value, length)) {
        return {ParseStatus::malformed, std::nullopt, 0, "invalid or overflowing array length"};
    }
    if (length < -1) {
        return {ParseStatus::malformed, std::nullopt, 0, "illegal negative array length"};
    }
    if (depth >= limits.max_depth) {
        return {ParseStatus::too_deep, std::nullopt, 0, "array nesting exceeds configured limit"};
    }
    if (length == -1) {
        return {ParseStatus::complete, RespValue::array(std::nullopt), line.next, ""};
    }

    const auto unsigned_length = static_cast<std::uint64_t>(length);
    if (unsigned_length > limits.max_array_size) {
        return {ParseStatus::oversized, std::nullopt, 0, "array exceeds configured element limit"};
    }
    if (unsigned_length > std::numeric_limits<std::size_t>::max()) {
        return {ParseStatus::oversized, std::nullopt, 0, "array length is not representable"};
    }

    const auto size = static_cast<std::size_t>(unsigned_length);
    std::vector<RespValue> values;
    values.reserve(size);
    std::size_t consumed = line.next;
    for (std::size_t index = 0; index < size; ++index) {
        const ValueResult item = parse_value(input.substr(consumed), depth + 1U, limits);
        if (item.status != ParseStatus::complete) {
            return {item.status, std::nullopt, 0, item.message};
        }
        values.push_back(*item.value);
        consumed += item.consumed;
    }

    return {ParseStatus::complete, RespValue::array(std::move(values)), consumed, ""};
}

[[nodiscard]] ValueResult parse_value(std::string_view input, std::size_t depth,
                                      const ParserLimits& limits) {
    if (input.empty()) {
        return {};
    }

    switch (input.front()) {
    case '+':
        return parse_line_value(input, false);
    case '-':
        return parse_line_value(input, true);
    case ':':
        return parse_resp_integer(input);
    case '$':
        return parse_bulk(input, limits);
    case '*':
        return parse_array(input, depth, limits);
    default:
        return {ParseStatus::malformed, std::nullopt, 0, "unknown RESP type prefix"};
    }
}

} // namespace

Parser::Parser(ParserLimits limits) : limits_(limits) {}

ParseStatus Parser::feed(std::string_view bytes) {
    if (buffer_oversized_) {
        return ParseStatus::oversized;
    }
    if (buffer_.size() > limits_.max_buffer_size ||
        bytes.size() > limits_.max_buffer_size - buffer_.size()) {
        buffer_oversized_ = true;
        return ParseStatus::oversized;
    }
    buffer_.append(bytes);
    return ParseStatus::incomplete;
}

ParseResult Parser::next() {
    if (buffer_oversized_) {
        return {ParseStatus::oversized, std::nullopt, "parser buffer exceeds configured limit"};
    }

    ValueResult result = parse_value(buffer_, 0, limits_);
    ParseResult public_result{result.status, std::move(result.value), result.message};
    if (result.status == ParseStatus::complete) {
        buffer_.erase(0, result.consumed);
    }
    return public_result;
}

void Parser::reset() noexcept {
    buffer_.clear();
    buffer_oversized_ = false;
}

std::size_t Parser::buffered_bytes() const noexcept { return buffer_.size(); }

const ParserLimits& Parser::limits() const noexcept { return limits_; }

} // namespace novacache::protocol
