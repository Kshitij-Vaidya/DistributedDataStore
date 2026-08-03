#pragma once

#include "novacache/protocol/resp_value.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace novacache::protocol {

enum class ParseStatus {
    complete,
    incomplete,
    malformed,
    oversized,
    too_deep,
};

struct ParserLimits {
    std::size_t max_bulk_size = 8U * 1024U * 1024U;
    std::size_t max_array_size = 1024U;
    std::size_t max_depth = 32U;
    std::size_t max_buffer_size = 16U * 1024U * 1024U;
};

struct ParseResult {
    ParseStatus status = ParseStatus::incomplete;
    std::optional<RespValue> value;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == ParseStatus::complete;
    }
};

class Parser {
  public:
    explicit Parser(ParserLimits limits = {});

    [[nodiscard]] ParseStatus feed(std::string_view bytes);
    [[nodiscard]] ParseResult next();

    void reset() noexcept;

    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    [[nodiscard]] const ParserLimits& limits() const noexcept;

  private:
    ParserLimits limits_;
    std::string buffer_;
    bool buffer_oversized_ = false;
};

} // namespace novacache::protocol
