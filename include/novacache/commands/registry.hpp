#pragma once

#include "novacache/protocol/resp_value.hpp"
#include "novacache/server/stats.hpp"
#include "novacache/store/store.hpp"

#include <cstddef>
#include <cstdint>
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

struct CommandContext {
    store::Store& store;
    server::Stats* stats = nullptr;
    std::string_view version = "0.1.0";
    std::uint16_t tcp_port = 0;
    std::size_t worker_count = 0;
};

class Registry {
  public:
    Registry();

    [[nodiscard]] protocol::RespValue execute(const protocol::RespValue& request,
                                              CommandContext& context) const;
    [[nodiscard]] protocol::RespValue execute(const protocol::RespValue& request,
                                              store::Store& store) const;
    [[nodiscard]] protocol::RespValue dispatch(const protocol::RespValue& request,
                                               CommandContext& context) const;
    [[nodiscard]] const CommandMetadata* metadata(std::string_view name) const;

  private:
    using Arguments = std::vector<std::string>;
    using Handler = std::function<protocol::RespValue(const Arguments&, CommandContext&)>;

    struct Entry {
        CommandMetadata metadata;
        Handler handler;
    };

    void add(std::string name, std::size_t minimum_arity, std::size_t maximum_arity,
             CommandAccess access, Handler handler);

    std::unordered_map<std::string, Entry> commands_;
};

} // namespace novacache::commands
