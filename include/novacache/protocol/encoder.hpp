#pragma once

#include "novacache/protocol/resp_value.hpp"

#include <string>

namespace novacache::protocol {

void append_encoded(const RespValue& value, std::string& output);

[[nodiscard]] std::string encode(const RespValue& value);

} // namespace novacache::protocol
