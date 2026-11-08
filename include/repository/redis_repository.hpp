#pragma once

#include "storage/redis/redis_client.hpp"

#include "repository/iredis_repository.hpp"

#include <memory>
#include <optional>
#include <string>

namespace run::repository
{

class RedisRepository final : public IRedisRepository
{
public:
    explicit RedisRepository(std::shared_ptr<run::redis::RedisSentinelClient> client) : m_client(std::move(client))
    {
    }

    net::awaitable<bool> Set(const std::string& key, const std::string& value, std::chrono::seconds ttl) override
    {
        co_return co_await m_client->SetAsync(key, value, ttl);
    }

    net::awaitable<std::optional<std::string>> Get(const std::string& key) override
    {
        co_return co_await m_client->GetAsync(key);
    }

    net::awaitable<bool> Delete(const std::string& key) override
    {
        co_return co_await m_client->DeleteAsync(key);
    }

private:
    std::shared_ptr<run::redis::RedisSentinelClient> m_client;
};

} // namespace run::repository
