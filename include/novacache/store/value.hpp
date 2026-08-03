#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <variant>

namespace novacache::store {

using StringList = std::deque<std::string>;
using StringSet = std::unordered_set<std::string>;
using Value = std::variant<std::string, std::int64_t, StringList, StringSet>;

} // namespace novacache::store
