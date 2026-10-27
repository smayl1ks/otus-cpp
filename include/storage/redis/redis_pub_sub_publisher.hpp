#pragma once

#include "service/running/radar_publisher.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

#include <memory>

namespace run::storage::redis
{

class RedisPubSubPublisher final : public service::running::IPubSubPublisher
{
public:
    explicit RedisPubSubPublisher(std::shared_ptr<ShardedRedisManager> redis_manager);
    ~RedisPubSubPublisher() override = default;

    boost::asio::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload) override;

private:
    std::shared_ptr<ShardedRedisManager> m_redis;
};

} // namespace run::storage::redis
