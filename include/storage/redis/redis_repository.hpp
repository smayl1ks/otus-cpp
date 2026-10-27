#pragma once

#include "repository/iredis_repository.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

#include <memory>

namespace run::repository
{

class RedisRepository final : public IRedisRepository
{
public:
    explicit RedisRepository(std::shared_ptr<storage::redis::ShardedRedisManager> redis_manager);
    ~RedisRepository() override = default;

    net::awaitable<bool> Set(const std::string& key, const std::string& value, std::chrono::seconds ttl) override;
    net::awaitable<std::optional<std::string>> Get(const std::string& key) override;
    net::awaitable<bool> Delete(const std::string& key) override;

private:
    std::shared_ptr<storage::redis::ShardedRedisManager> m_redis;
};

} // namespace run::repository
