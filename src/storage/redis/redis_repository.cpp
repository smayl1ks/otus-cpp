#include "storage/redis/redis_repository.hpp"

namespace run::repository
{

RedisRepository::RedisRepository(std::shared_ptr<storage::redis::ShardedRedisManager> redis_manager)
    : m_redis(std::move(redis_manager))
{
}

net::awaitable<bool> RedisRepository::Set(const std::string& key, const std::string& value, std::chrono::seconds ttl)
{
    if (!m_redis)
    {
        co_return false;
    }
    co_return co_await m_redis->SetAsync(key, value, ttl);
}

net::awaitable<std::optional<std::string>> RedisRepository::Get(const std::string& key)
{
    if (!m_redis)
    {
        co_return std::nullopt;
    }
    co_return co_await m_redis->GetAsync(key);
}

net::awaitable<bool> RedisRepository::Delete(const std::string& key)
{
    if (!m_redis)
    {
        co_return false;
    }
    co_return co_await m_redis->DeleteAsync(key);
}

} // namespace run::repository
