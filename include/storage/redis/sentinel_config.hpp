#pragma once

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace run::storage::redis
{

struct SentinelNode
{
    std::string host{"127.0.0.1"};
    std::string port{"26379"};
};

struct RedisConfig
{
    std::string master_name{"mymaster"};
    std::vector<SentinelNode> sentinels;
    std::string password;
    std::chrono::milliseconds connection_timeout{100};
    std::chrono::milliseconds command_timeout{50};
    std::size_t num_shards{1};
    std::size_t pool_size{2};
    std::chrono::seconds ttl_refresh_interval{30};

    // Dedicated Pub/Sub fleet (radar). Priority:
    //   1. pubsub_hosts non-empty  -> client-side sharded fleet, channel routed
    //                                 by hash(h3_zone) % hosts.size()
    //   2. pubsub_host non-empty   -> single dedicated instance
    //   3. both empty              -> legacy mode, publish via sharded masters
    std::vector<std::pair<std::string, std::string>> pubsub_hosts; // parsed "host", "port"
    std::string pubsub_host;
    std::string pubsub_port{"6379"};
    std::string pubsub_password;
};

struct BufferConfig
{
    std::size_t in_memory_capacity{4096};
    std::size_t drain_batch_size{256};
};

struct DeferredTrackEvent
{
    uint64_t run_id{0};
    double latitude{0.0};
    double longitude{0.0};
    double speed{0.0};
    int64_t timestamp{0};
    double delta_meters{0.0};
    int32_t delta_points{0};
};

/**
 * @brief Formats shard master name.
 * 
 * When total_shards <= 1, returns base_name ("mymaster") for backward compatibility
 * with single-master setups (e.g. docker-compose, standalone Sentinel).
 * When total_shards > 1, returns "base_name-0", "base_name-1", etc.
 */
inline std::string GetShardMasterName(std::string_view base_name, std::size_t shard_idx, std::size_t total_shards)
{
    if (base_name.empty())
    {
        base_name = "mymaster";
    }
    if (total_shards <= 1)
    {
        return std::string(base_name);
    }
    return std::string(base_name) + "-" + std::to_string(shard_idx);
}

inline std::vector<SentinelNode> ParseSentinelNodes(std::string_view sentinels_str)
{
    std::vector<SentinelNode> nodes;
    std::stringstream ss{std::string(sentinels_str)};
    std::string item;

    while (std::getline(ss, item, ','))
    {
        const auto start = item.find_first_not_of(" \t\r\n");
        const auto end = item.find_last_not_of(" \t\r\n");
        if (start == std::string::npos)
        {
            continue;
        }
        item = item.substr(start, end - start + 1);

        const auto colon_pos = item.find(':');
        if (colon_pos == std::string::npos)
        {
            nodes.push_back(SentinelNode{.host = item, .port = "26379"});
        }
        else
        {
            nodes.push_back(SentinelNode{
                .host = item.substr(0, colon_pos),
                .port = item.substr(colon_pos + 1),
            });
        }
    }

    return nodes;
}

/**
 * @brief Parses a comma-separated list of pub/sub instance addresses.
 * Each entry is "host[:port]"; missing port falls back to default_port.
 */
std::vector<std::pair<std::string, std::string>> ParsePubSubHosts(std::string_view hosts_string,
                                                                  std::string_view default_port = "6379");

/**
 * @brief Overrides RedisConfig values with environment variables if present.
 * Supported env vars:
 *   - REDIS_NUM_SHARDS
 *   - REDIS_POOL_SIZE
 *   - REDIS_MASTER_NAME
 *   - REDIS_SENTINELS (comma-separated host:port)
 *   - REDIS_PASSWORD
 *   - REDIS_CONNECTION_TIMEOUT_MS
 *   - REDIS_COMMAND_TIMEOUT_MS
 *   - REDIS_TTL_REFRESH_INTERVAL_SEC
 */
RedisConfig LoadRedisConfigFromEnv(RedisConfig base = {});

/**
 * @brief Overrides BufferConfig values with environment variables if present.
 * Supported env vars:
 *   - BUFFER_IN_MEMORY_CAPACITY
 *   - BUFFER_DRAIN_BATCH_SIZE
 */
BufferConfig LoadBufferConfigFromEnv(BufferConfig base = {});

/**
 * @brief Loads both RedisConfig and BufferConfig from config file (INI) and env.
 * Environment variables take precedence over config file settings.
 */
std::pair<RedisConfig, BufferConfig> LoadStorageConfig(const std::string& config_file = "");

} // namespace run::storage::redis
