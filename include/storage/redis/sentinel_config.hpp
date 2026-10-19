#pragma once

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
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

inline std::string GetShardMasterName(std::string_view base_name, std::size_t shard_idx, std::size_t total_shards)
{
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

} // namespace run::storage::redis