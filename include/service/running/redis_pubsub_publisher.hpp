#pragma once

#include "service/running/radar_publisher.hpp"
#include "storage/redis/redis_client.hpp"
#include "storage/redis/shard_router.hpp"

#include <charconv>
#include <memory>
#include <string>
#include <string_view>

namespace run::service::running
{

inline std::size_t RouteRadarChannel(std::size_t shard_count, std::string_view channel)
{
    constexpr std::string_view kZonePrefix = "kazan:zone:";
    if (shard_count > 1 && channel.starts_with(kZonePrefix))
    {
        const std::string_view digits = channel.substr(kZonePrefix.size());
        uint64_t zone = 0;
        if (std::from_chars(digits.data(), digits.data() + digits.size(), zone).ec == std::errc{})
        {
            return run::redis::ShardRouter::Hash64(zone) % shard_count;
        }
    }
    return run::redis::ShardRouter::HashString(channel) % shard_count;
}

/// Pub/Sub publisher over RedisSentinelClient using the injected channel router
class RedisPubSubPublisher final : public IPubSubPublisher
{
public:
    explicit RedisPubSubPublisher(std::shared_ptr<run::redis::RedisSentinelClient> client) : m_client(std::move(client))
    {
    }

    net::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload) override
    {
        co_return (co_await m_client->PublishAsync(channel, payload)).has_value();
    }

private:
    std::shared_ptr<run::redis::RedisSentinelClient> m_client;
};

} // namespace run::service::running
