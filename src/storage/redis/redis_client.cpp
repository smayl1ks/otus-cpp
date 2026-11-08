#include "storage/redis/redis_client.hpp"

#include <boost/redis/src.hpp>

#include "logger/logger.hpp"
#include "storage/redis/shard_manager.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/redis/ignore.hpp>
#include <boost/redis/resp3/type.hpp>
#include <boost/redis/response.hpp>
#include <charconv>
#include <fmt/format.h>

namespace run::redis
{

namespace
{

using namespace std::chrono_literals;

constexpr auto kCommandTimeout = 5s;
constexpr auto kHeavyTimeout = 10s;

using Node = boost::redis::resp3::node;
using Generic = boost::redis::generic_response;

bool IsErrorNode(const Node& node)
{
    return node.data_type == boost::redis::resp3::type::simple_error ||
           node.data_type == boost::redis::resp3::type::blob_error;
}

// Typed response adapters silently turn RESP3 errors into garbage values, so
// all reads go through generic responses with explicit node checks.

/// nil / error -> nullopt, otherwise the single bulk payload.
std::optional<std::string> SingleBulk(Generic& resp)
{
    if (!resp.has_value() || resp->empty())
    {
        return std::nullopt;
    }
    const Node& head = (*resp)[0];
    if (IsErrorNode(head) || head.data_type == boost::redis::resp3::type::null)
    {
        return std::nullopt;
    }
    return head.value;
}

/// Children of the head aggregate (LRANGE / SMEMBERS); error -> empty.
std::vector<std::string> BulkArray(Generic& resp)
{
    std::vector<std::string> out;
    if (!resp.has_value() || resp->empty())
    {
        return out;
    }
    const Node& head = (*resp)[0];
    if (IsErrorNode(head) || head.data_type == boost::redis::resp3::type::null)
    {
        return out;
    }
    const std::size_t count = std::min<std::size_t>(head.aggregate_size, resp->size() - 1);
    out.reserve(count);
    for (std::size_t i = 1; i <= count; ++i)
    {
        out.push_back(std::move((*resp)[i].value));
    }
    return out;
}

/// RESP3 integer reply (SCARD, EVAL); error -> nullopt.
std::optional<int64_t> IntegerReply(Generic& resp)
{
    const auto text = SingleBulk(resp);
    if (!text)
    {
        return std::nullopt;
    }
    int64_t value = 0;
    const auto* first = text->data();
    if (std::from_chars(first, first + text->size(), value).ec != std::errc{})
    {
        return std::nullopt;
    }
    return value;
}

/// Consumes one HGETALL reply (head node + alternating field/value children)
/// starting at `pos`, filling `fields`.
void ConsumeHGetAllReply(const std::vector<Node>& nodes, std::size_t& pos,
                         std::unordered_map<std::string, std::string>& fields)
{
    const Node& head = nodes[pos++];
    if (head.data_type == boost::redis::resp3::type::map || head.data_type == boost::redis::resp3::type::array)
    {
        for (std::size_t pair = 0; pair < head.aggregate_size && pos + 1 < nodes.size(); ++pair, pos += 2)
        {
            fields[std::move(nodes[pos].value)] = std::move(nodes[pos + 1].value);
        }
    }
    // null / error / other: the key stays an empty map
}

template <typename RoutingKeyT, typename Response>
boost::asio::awaitable<boost::system::error_code>
ExecOn(const std::shared_ptr<ShardManager>& manager, RoutingKeyT&& routing_key, const boost::redis::request& request,
       Response& response, std::chrono::milliseconds timeout)
{
    return manager->Execute(std::forward<RoutingKeyT>(routing_key), request, response, timeout);
}

std::string FormatTtl(std::chrono::seconds ttl)
{
    return std::to_string(ttl.count());
}

} // namespace

// ---------------------------------------------------------------------------

struct RedisSentinelClient::Impl
{
    std::shared_ptr<ShardManager> manager;
    RedisSentinelClientOptions options;
};

RedisSentinelClient::RedisSentinelClient(boost::asio::any_io_executor& io_context, const SentinelConfig& config,
                                         RedisSentinelClientOptions options)
    : m_impl{std::make_unique<Impl>()}
{
    m_impl->options = std::move(options);

    // use_setup replaces the built-in HELLO, so RESP3 negotiation (and AUTH,
    // once Redis auth is re-enabled) must ride in the request. The setup runs
    // after every (re)connection on every shard master; the application may
    // extend it (e.g. FUNCTION LOAD of its own function library) without the
    // client knowing what the commands mean.
    boost::redis::request setup;
    if (config.password.empty())
    {
        setup.push("HELLO", "3");
    }
    else
    {
        setup.push("HELLO", "3", "AUTH", "default", config.password);
    }
    setup.append(m_impl->options.extra_setup_commands);
    m_impl->manager = std::make_shared<ShardManager>(io_context, config, &setup);
}

RedisSentinelClient::~RedisSentinelClient() = default;

// --- Generic key-value -------------------------------------------------------

boost::asio::awaitable<bool> RedisSentinelClient::SetAsync(std::string key, std::string value, std::chrono::seconds ttl)
{
    boost::redis::request req;
    req.push("SET", key, value, "EX", FormatTtl(ttl));

    boost::redis::ignore_t resp; // RESP3 errors surface in the error code
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("SET {} failed: {}", key, ec.message());
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<bool> RedisSentinelClient::SetAsync(std::string key, std::string value)
{
    boost::redis::request req;
    req.push("SET", key, value);

    boost::redis::ignore_t resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("SET {} failed: {}", key, ec.message());
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<std::optional<bool>> RedisSentinelClient::SetNxAsync(std::string key, std::string value,
                                                                            std::chrono::seconds ttl)
{
    boost::redis::request req;
    req.push("SET", key, value, "EX", FormatTtl(ttl), "NX");

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("SETNX {} failed: {}", key, ec.message());
        co_return std::nullopt;
    }
    // SET NX replies nil when the key already existed.
    co_return SingleBulk(resp).has_value();
}

boost::asio::awaitable<std::optional<std::string>> RedisSentinelClient::GetAsync(std::string key)
{
    boost::redis::request req;
    req.push("GET", key);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("GET {} failed: {}", key, ec.message());
        co_return std::nullopt;
    }
    co_return SingleBulk(resp);
}

boost::asio::awaitable<bool> RedisSentinelClient::DeleteAsync(std::string key)
{
    boost::redis::request req;
    req.push("DEL", key);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("DEL {} failed: {}", key, ec.message());
        co_return false;
    }
    const auto deleted = IntegerReply(resp);
    co_return deleted.has_value() && *deleted > 0;
}

boost::asio::awaitable<bool> RedisSentinelClient::ExistsAsync(std::string key)
{
    boost::redis::request req;
    req.push("EXISTS", key);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("EXISTS {} failed: {}", key, ec.message());
        co_return false;
    }
    const auto exists = IntegerReply(resp);
    co_return exists.has_value() && *exists > 0;
}

// --- Generic key-value (atomic consume) ---------------------------------------

boost::asio::awaitable<std::optional<std::string>> RedisSentinelClient::GetDelAsync(std::string key)
{
    boost::redis::request req;
    req.push("GETDEL", key);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("GETDEL {} failed: {}", key, ec.message());
        co_return std::nullopt;
    }
    co_return SingleBulk(resp);
}

// --- Hashes ---------------------------------------------------------------------

boost::asio::awaitable<bool> RedisSentinelClient::HSetAsync(std::string key, std::string field, std::string value,
                                                            std::optional<std::chrono::seconds> ttl)
{
    boost::redis::request req;
    req.push("HSET", key, field, value);
    if (ttl.has_value())
    {
        req.push("EXPIRE", key, FormatTtl(*ttl));
    }

    boost::redis::ignore_t resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("HSET {} failed: {}", key, ec.message());
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<bool>
RedisSentinelClient::HSetMultipleAsync(std::string key,
                                       std::span<const std::pair<std::string, std::string>> field_values,
                                       std::optional<std::chrono::seconds> ttl)
{
    std::vector<std::string> flattened;
    flattened.reserve(field_values.size() * 2);
    for (const auto& [field, value] : field_values)
    {
        flattened.push_back(field);
        flattened.push_back(value);
    }
    co_return co_await HSetMultipleAsync(std::move(key), flattened, ttl);
}

boost::asio::awaitable<bool> RedisSentinelClient::HSetMultipleAsync(std::string key,
                                                                    std::span<const std::string> flattened,
                                                                    std::optional<std::chrono::seconds> ttl)
{
    boost::redis::request req;
    req.push_range("HSET", key, flattened.begin(), flattened.end());
    if (ttl.has_value())
    {
        req.push("EXPIRE", key, FormatTtl(*ttl));
    }

    boost::redis::ignore_t resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("HSET {} failed: {}", key, ec.message());
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<std::optional<std::string>> RedisSentinelClient::HGetAsync(const std::string& key,
                                                                                  const std::string& field)
{
    boost::redis::request req;
    req.push("HGET", key, field);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("HGET {} failed: {}", key, ec.message());
        co_return std::nullopt;
    }
    co_return SingleBulk(resp);
}

boost::asio::awaitable<std::unordered_map<std::string, std::string>>
RedisSentinelClient::HGetAllAsync(const std::string& key)
{
    std::unordered_map<std::string, std::string> fields;

    boost::redis::request req;
    req.push("HGETALL", key);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("HGETALL {} failed: {}", key, ec.message());
        co_return fields;
    }
    if (resp.has_value() && !resp->empty())
    {
        std::size_t pos = 0;
        ConsumeHGetAllReply(*resp, pos, fields);
    }
    co_return fields;
}

boost::asio::awaitable<std::vector<std::unordered_map<std::string, std::string>>>
RedisSentinelClient::HGetAllManyAsync(const std::vector<std::string>& keys)
{
    std::vector<std::unordered_map<std::string, std::string>> results(keys.size());
    if (keys.empty())
    {
        co_return results;
    }

    // Group keys per shard, one pipelined request each.
    const std::size_t shard_count = m_impl->manager->GetShardCount();
    std::vector<std::vector<std::size_t>> per_shard(shard_count);
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        per_shard[m_impl->manager->GetShardIndex(keys[i])].push_back(i);
    }

    for (std::size_t shard = 0; shard < shard_count; ++shard)
    {
        const auto& indexes = per_shard[shard];
        if (indexes.empty())
        {
            continue;
        }

        boost::redis::request req;
        for (const auto i : indexes)
        {
            req.push("HGETALL", keys[i]);
        }

        Generic resp;
        const auto ec = co_await m_impl->manager->GetShard(shard)->AsyncExec(req, resp, kHeavyTimeout);
        if (ec)
        {
            LOG_WARN("Pipelined HGETALL of {} keys on shard {} failed: {}", indexes.size(), shard, ec.message());
            continue;
        }
        if (!resp.has_value())
        {
            continue;
        }

        std::size_t pos = 0;
        for (const auto i : indexes)
        {
            if (pos >= resp->size())
            {
                break;
            }
            ConsumeHGetAllReply(*resp, pos, results[i]);
        }
    }

    co_return results;
}

// --- Lists ------------------------------------------------------------------------

boost::asio::awaitable<bool> RedisSentinelClient::RPushRangeAsync(const std::string& key,
                                                                  const std::vector<std::string>& values,
                                                                  std::optional<std::chrono::seconds> ttl, bool rewrite)
{
    if (values.empty())
    {
        co_return true;
    }

    boost::redis::request req;
    if (rewrite)
    {
        // DEL+RPUSH in one request: re-uploading the same chunk replaces it
        // (idempotent per chunk, closes the old R2 duplication issue).
        req.push("DEL", key);
    }
    req.push_range("RPUSH", key, values.begin(), values.end());
    if (ttl.has_value())
    {
        req.push("EXPIRE", key, FormatTtl(*ttl));
    }

    boost::redis::ignore_t resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kHeavyTimeout);
    if (ec)
    {
        LOG_WARN("RPUSH {} failed: {}", key, ec.message());
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<std::vector<std::string>> RedisSentinelClient::LRangeAsync(const std::string& key, int64_t start,
                                                                                  int64_t stop)
{
    boost::redis::request req;
    req.push("LRANGE", key, std::to_string(start), std::to_string(stop));

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kHeavyTimeout);
    if (ec)
    {
        LOG_WARN("LRANGE {} failed: {}", key, ec.message());
        co_return std::vector<std::string>{};
    }
    co_return BulkArray(resp);
}

// --- Sets ---------------------------------------------------------------------------

boost::asio::awaitable<bool> RedisSentinelClient::SAddAsync(const std::string& key, const std::string& member,
                                                            std::optional<std::chrono::seconds> ttl)
{
    boost::redis::request req;
    req.push("SADD", key, member);
    if (ttl.has_value())
    {
        req.push("EXPIRE", key, FormatTtl(*ttl));
    }

    boost::redis::ignore_t resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("SADD {} failed: {}", key, ec.message());
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<std::vector<std::string>> RedisSentinelClient::SMembersAsync(const std::string& key)
{
    boost::redis::request req;
    req.push("SMEMBERS", key);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("SMEMBERS {} failed: {}", key, ec.message());
        co_return std::vector<std::string>{};
    }
    co_return BulkArray(resp);
}

boost::asio::awaitable<std::optional<int64_t>> RedisSentinelClient::SCardAsync(const std::string& key)
{
    boost::redis::request req;
    req.push("SCARD", key);

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, key, req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("SCARD {} failed: {}", key, ec.message());
        co_return std::nullopt;
    }
    co_return IntegerReply(resp);
}

// --- Batches --------------------------------------------------------------------------

boost::asio::awaitable<bool> RedisSentinelClient::ExecBatchAsync(const std::string& route_key,
                                                                 const boost::redis::request& commands)
{
    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, route_key, commands, resp, kHeavyTimeout);
    if (ec)
    {
        LOG_WARN("Batch on {} failed: {}", route_key, ec.message());
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<std::optional<int64_t>>
RedisSentinelClient::FcallAsync(std::string routing_key, std::string function, std::span<const std::string> keys,
                                std::span<const std::string> args)
{
    // FCALL <function> <numkeys> <keys...> <args...> as one command; numkeys
    // is keys.size() by the Redis Functions contract.
    std::vector<std::string> tail;
    tail.reserve(keys.size() + args.size() + 2);
    tail.push_back(function);
    tail.push_back(std::to_string(keys.size()));
    tail.insert(tail.end(), keys.begin(), keys.end());
    tail.insert(tail.end(), args.begin(), args.end());

    boost::redis::request req;
    req.push_range("FCALL", tail.begin(), tail.end());

    Generic resp;
    const auto ec = co_await ExecOn(m_impl->manager, routing_key, req, resp, kHeavyTimeout);
    if (ec)
    {
        LOG_WARN("FCALL {} on {} failed: {}", function, routing_key, ec.message());
        co_return std::nullopt;
    }
    co_return IntegerReply(resp);
}

// --- Pub/Sub -----------------------------------------------------------------------------

boost::asio::awaitable<std::optional<int64_t>> RedisSentinelClient::PublishAsync(std::string channel,
                                                                                  std::string payload)
{
    const std::size_t shard_count = m_impl->manager->GetShardCount();
    std::size_t shard = 0;
    if (shard_count > 1)
    {
        if (m_impl->options.channel_router)
        {
            shard = m_impl->options.channel_router(shard_count, channel) % shard_count;
        }
        else
        {
            shard = ShardRouter::HashString(channel) % shard_count;
        }
    }

    boost::redis::request req;
    req.push("PUBLISH", channel, payload);

    Generic resp;
    const auto ec = co_await m_impl->manager->GetShard(shard)->AsyncExec(req, resp, kCommandTimeout);
    if (ec)
    {
        LOG_WARN("PUBLISH {} failed: {}", channel, ec.message());
        co_return std::nullopt;
    }
    co_return IntegerReply(resp);
}

} // namespace run::redis
