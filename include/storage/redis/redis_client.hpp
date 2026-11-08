#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/redis/request.hpp>

#include "storage/redis/sentinel_config.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace run::redis
{

/// Application-provided configuration injected into RedisSentinelClient
struct RedisSentinelClientOptions
{
    /// Commands executed on every shard master after every (re)connection
    /// (e.g., FUNCTION LOAD). Failures cause reconnection retries
    boost::redis::request extra_setup_commands{};

    /// Channel routing policy: maps (shard_count, channel) to a shard index
    std::function<std::size_t(std::size_t shard_count, std::string_view channel)> channel_router;
};

/**
 * @brief Generic Redis command API over a sharded Sentinel fleet.
 */
class RedisSentinelClient
{
public:
    RedisSentinelClient(boost::asio::any_io_executor& io_context, const SentinelConfig& config,
                        RedisSentinelClientOptions options = {});
    ~RedisSentinelClient();

    RedisSentinelClient(const RedisSentinelClient&) = delete;
    RedisSentinelClient& operator=(const RedisSentinelClient&) = delete;

    boost::asio::awaitable<bool> SetAsync(std::string key, std::string value, std::chrono::seconds ttl);
    boost::asio::awaitable<bool> SetAsync(std::string key, std::string value);

    /// SET NX EX: true = acquired, false = key existed, nullopt = transport error
    boost::asio::awaitable<std::optional<bool>> SetNxAsync(std::string key, std::string value,
                                                           std::chrono::seconds ttl);

    boost::asio::awaitable<std::optional<std::string>> GetAsync(std::string key);

    boost::asio::awaitable<std::optional<std::string>> GetDelAsync(std::string key);

    boost::asio::awaitable<bool> DeleteAsync(std::string key);
    boost::asio::awaitable<bool> ExistsAsync(std::string key);

    // --- Hashes ----
    boost::asio::awaitable<bool> HSetAsync(std::string key, std::string field, std::string value,
                                           std::optional<std::chrono::seconds> ttl);
    boost::asio::awaitable<bool> HSetMultipleAsync(std::string key,
                                                   std::span<const std::pair<std::string, std::string>> field_values,
                                                   std::optional<std::chrono::seconds> ttl);
    boost::asio::awaitable<bool> HSetMultipleAsync(std::string key, std::span<const std::string> flattened_field_values,
                                                   std::optional<std::chrono::seconds> ttl);
    boost::asio::awaitable<std::optional<std::string>> HGetAsync(const std::string& key, const std::string& field);
    boost::asio::awaitable<std::unordered_map<std::string, std::string>> HGetAllAsync(const std::string& key);

    /// Pipelined HGETALL of many keys, grouped per shard (one request per shard)
    boost::asio::awaitable<std::vector<std::unordered_map<std::string, std::string>>>
    HGetAllManyAsync(const std::vector<std::string>& keys);

    // --- Lists ---
    /// Pushes a range of values. If rewrite=true, replaces the list (idempotent); otherwise appends
    boost::asio::awaitable<bool> RPushRangeAsync(const std::string& key, const std::vector<std::string>& values,
                                                 std::optional<std::chrono::seconds> ttl, bool rewrite = false);

    boost::asio::awaitable<std::vector<std::string>> LRangeAsync(const std::string& key, int64_t start, int64_t stop);

    boost::asio::awaitable<bool> SAddAsync(const std::string& key, const std::string& member,
                                           std::optional<std::chrono::seconds> ttl);
    boost::asio::awaitable<std::vector<std::string>> SMembersAsync(const std::string& key);

    /// nullopt on transport error, otherwise the cardinality
    boost::asio::awaitable<std::optional<int64_t>> SCardAsync(const std::string& key);

    /// Runs a pre-built multi-command request in one round trip on the shard
    /// owning route_key
    boost::asio::awaitable<bool> ExecBatchAsync(const std::string& route_key, const boost::redis::request& commands);

    /// FCALL <function> <numkeys> <keys...> <args...> on the shard owning
    /// routing_key; numkeys is keys.size()
    boost::asio::awaitable<std::optional<int64_t>> FcallAsync(std::string routing_key, std::string function,
                                                              std::span<const std::string> keys,
                                                              std::span<const std::string> args);

    /// PUBLISH on the shard chosen by options.channel_router
    boost::asio::awaitable<std::optional<int64_t>> PublishAsync(std::string channel, std::string payload);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace run::redis
