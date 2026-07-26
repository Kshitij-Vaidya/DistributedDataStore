#include "novacache/version.hpp"

namespace novacache {

std::string_view version() noexcept { return NOVACACHE_VERSION_STRING; }

} // namespace novacache
