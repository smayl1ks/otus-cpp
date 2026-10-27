#include "storage/redis/redis_pub_sub_publisher.hpp"

namespace run::storage::redis
{

RedisPubSubPublisher::RedisPubSubPublisher(std::shared_ptr<ShardedRedisManager> redis_manager)
    : m_redis(std::move(redis_manager))
{
}

boost::asio::awaitable<bool>
RedisPubSubPublisher::PublishAsync(const std::string& channel, const std::string& payload)
{
    if (!m_redis)
    {
        co_return false;
    }
    co_return co_await m_redis->PublishAsync(channel, payload);
}

} // namespace run::storage::redis
