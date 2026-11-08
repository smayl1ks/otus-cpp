#pragma once

#include "storage/redis/sentinel_config.hpp"

#include <boost/redis/config.hpp>
#include <boost/redis/request.hpp>

#include <fmt/format.h>
#include <string>

namespace run::redis
{

/// Sentinel master name of a shard: "<master_prefix>-<index>" (master-0, ...)
inline std::string MakeMasterName(const std::string& master_prefix, std::size_t shard_index)
{
    return fmt::format("{}-{}", master_prefix, shard_index);
}

/// Creates a boost-redis config for a shard master (Sentinel-resolved or direct)
inline boost::redis::config MakeRedisConfig(const SentinelConfig& redis_config, std::size_t shard_index,
                                            const boost::redis::request* setup_commands = nullptr)
{
    boost::redis::config config;

    if (setup_commands == nullptr && !redis_config.password.empty())
    {
        config.password = redis_config.password;
    }

    if (redis_config.sentinel_nodes.empty())
    {
        config.addr.host = redis_config.direct_host;
        config.addr.port = redis_config.direct_port;
    }
    else
    {
        config.sentinel.master_name = MakeMasterName(redis_config.master_prefix, shard_index);

        for (const auto& node : redis_config.sentinel_nodes)
        {
            config.sentinel.addresses.push_back({node.host, node.port});
        }
    }

    if (setup_commands != nullptr)
    {
        config.use_setup = true;
        config.setup = *setup_commands;
    }

    config.connect_timeout = redis_config.connection_timeout;
    return config;
}

} // namespace run::redis
