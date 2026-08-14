#include "novacache/commands/registry.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

namespace novacache::commands {
namespace {

using protocol::RespValue;

constexpr std::string_view wrong_arity = "ERR wrong number of arguments";
constexpr std::string_view invalid_integer = "ERR value is not an integer or out of range";
constexpr std::string_view wrong_type =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

[[nodiscard]] std::string normalize(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const char character : name) {
        if (character >= 'a' && character <= 'z') {
            result.push_back(static_cast<char>(character - 'a' + 'A'));
        } else {
            result.push_back(character);
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view text) {
    if (text.empty() || text.front() == '+') {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool expiration_is_representable(const std::int64_t seconds) {
    if (seconds <= 0) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto room = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::time_point::max() - now)
                          .count();
    return seconds <= room;
}

[[nodiscard]] RespValue ping(const std::vector<std::string>& arguments, CommandContext&) {
    if (arguments.size() == 1U) {
        return RespValue::simple("PONG");
    }
    return RespValue::bulk(arguments[1]);
}

[[nodiscard]] RespValue echo(const std::vector<std::string>& arguments, CommandContext&) {
    return RespValue::bulk(arguments[1]);
}

[[nodiscard]] RespValue set(const std::vector<std::string>& arguments, CommandContext& context) {
    context.store.set(arguments[1], std::string{arguments[2]});
    return RespValue::simple("OK");
}

[[nodiscard]] RespValue get(const std::vector<std::string>& arguments, CommandContext& context) {
    const std::optional<store::Value> value = context.store.get(arguments[1]);
    if (!value.has_value()) {
        return RespValue::bulk(std::nullopt);
    }
    if (const auto* const string = std::get_if<std::string>(&*value); string != nullptr) {
        return RespValue::bulk(*string);
    }
    return RespValue::error(std::string{wrong_type});
}

[[nodiscard]] RespValue del(const std::vector<std::string>& arguments, CommandContext& context) {
    std::vector<std::string_view> keys;
    keys.reserve(arguments.size() - 1U);
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        keys.emplace_back(arguments[index]);
    }
    return RespValue::integer(static_cast<std::int64_t>(context.store.del_many(keys)));
}

[[nodiscard]] RespValue exists(const std::vector<std::string>& arguments, CommandContext& context) {
    std::vector<std::string_view> keys;
    keys.reserve(arguments.size() - 1U);
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        keys.emplace_back(arguments[index]);
    }
    return RespValue::integer(static_cast<std::int64_t>(context.store.exists_many(keys)));
}

[[nodiscard]] RespValue expire(const std::vector<std::string>& arguments, CommandContext& context) {
    const std::optional<std::int64_t> seconds = parse_int64(arguments[2]);
    if (!seconds.has_value() || !expiration_is_representable(*seconds)) {
        return RespValue::error(std::string{invalid_integer});
    }
    const bool changed = context.store.expire(arguments[1], std::chrono::seconds{*seconds});
    return RespValue::integer(changed ? 1 : 0);
}

[[nodiscard]] RespValue ttl(const std::vector<std::string>& arguments, CommandContext& context) {
    return RespValue::integer(context.store.ttl(arguments[1]));
}

[[nodiscard]] RespValue keys(const std::vector<std::string>& arguments, CommandContext& context) {
    store::KeysResult result = context.store.keys(arguments[1]);
    if (result.status != store::KeysStatus::ok) {
        return RespValue::error("ERR unsupported pattern");
    }

    std::vector<RespValue> values;
    values.reserve(result.keys.size());
    for (std::string& key : result.keys) {
        values.push_back(RespValue::bulk(std::move(key)));
    }
    return RespValue::array(std::move(values));
}

[[nodiscard]] RespValue info(const std::vector<std::string>&, CommandContext& context) {
    if (context.stats == nullptr) {
        return RespValue::bulk(std::string{});
    }
    return RespValue::bulk(context.stats->render_info(
        context.version, context.tcp_port, context.worker_count, context.store.shard_count()));
}

[[nodiscard]] std::optional<std::vector<std::string>> extract_arguments(const RespValue& request) {
    const auto* const array = std::get_if<protocol::Array>(&request.storage());
    if (array == nullptr || !array->value.has_value() || array->value->empty()) {
        return std::nullopt;
    }

    std::vector<std::string> arguments;
    arguments.reserve(array->value->size());
    for (const RespValue& item : *array->value) {
        const auto* const bulk = std::get_if<protocol::BulkString>(&item.storage());
        if (bulk == nullptr || !bulk->value.has_value()) {
            return std::nullopt;
        }
        arguments.push_back(*bulk->value);
    }
    return arguments;
}

} // namespace

Registry::Registry() {
    add("PING", 1, 2, CommandAccess::read_only, ping);
    add("ECHO", 2, 2, CommandAccess::read_only, echo);
    add("SET", 3, 3, CommandAccess::write, set);
    add("GET", 2, 2, CommandAccess::read_only, get);
    add("DEL", 2, std::numeric_limits<std::size_t>::max(), CommandAccess::write, del);
    add("EXISTS", 2, std::numeric_limits<std::size_t>::max(), CommandAccess::read_only, exists);
    add("EXPIRE", 3, 3, CommandAccess::write, expire);
    add("TTL", 2, 2, CommandAccess::read_only, ttl);
    add("KEYS", 2, 2, CommandAccess::read_only, keys);
    add("INFO", 1, 2, CommandAccess::read_only, info);
}

RespValue Registry::execute(const RespValue& request, CommandContext& context) const {
    const std::optional<Arguments> arguments = extract_arguments(request);
    if (!arguments.has_value()) {
        return RespValue::error("ERR Protocol error");
    }

    const auto iterator = commands_.find(normalize(arguments->front()));
    if (iterator == commands_.end()) {
        return RespValue::error("ERR unknown command");
    }
    const CommandMetadata& metadata = iterator->second.metadata;
    if (arguments->size() < metadata.minimum_arity || arguments->size() > metadata.maximum_arity) {
        return RespValue::error(std::string{wrong_arity});
    }
    return iterator->second.handler(*arguments, context);
}

RespValue Registry::execute(const RespValue& request, store::Store& store) const {
    CommandContext context{.store = store};
    return execute(request, context);
}

RespValue Registry::dispatch(const RespValue& request, CommandContext& context) const {
    return execute(request, context);
}

const CommandMetadata* Registry::metadata(const std::string_view name) const {
    const auto iterator = commands_.find(normalize(name));
    if (iterator == commands_.end()) {
        return nullptr;
    }
    return &iterator->second.metadata;
}

void Registry::add(std::string name, const std::size_t minimum_arity,
                   const std::size_t maximum_arity, const CommandAccess access, Handler handler) {
    std::string key = normalize(name);
    const bool mutating = access == CommandAccess::write;
    commands_.emplace(std::move(key),
                      Entry{CommandMetadata{std::move(name), minimum_arity, maximum_arity, access,
                                            mutating, mutating},
                            std::move(handler)});
}

} // namespace novacache::commands
