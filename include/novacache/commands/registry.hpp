#pragma once

#include "novacache/protocol/resp_value.hpp"
#include "novacache/store/store.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace novacache::commands {

enum class CommandAccess {
    read_only,
    write,
};

struct CommandMetadata {
    std::string name;
    std::size_t minimum_arity = 0;
    std::size_t maximum_arity = 0;
    CommandAccess access = CommandAccess::read_only;
    bool persistent = false;
    bool replicated = false;
};

class Registry {
  public:
    Registry();

    [[nodiscard]] protocol::RespValue execute(const protocol::RespValue& request,
                                              store::Store& store) const;
    [[nodiscard]] protocol::RespValue dispatch(const protocol::RespValue& request,
                                               store::Store& store) const;
    [[nodiscard]] const CommandMetadata* metadata(std::string_view name) const;

  private:
    using Arguments = std::vector<std::string>;
    using Handler = std::function<protocol::RespValue(const Arguments&, store::Store&)>;

    struct Entry {
        CommandMetadata metadata;
        Handler handler;
    };

    void add(std::string name, std::size_t minimum_arity, std::size_t maximum_arity,
             CommandAccess access, Handler handler);

    std::unordered_map<std::string, Entry> commands_;
};

} // namespace novacache::commands
