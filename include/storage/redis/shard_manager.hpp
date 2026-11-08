#pragma once
#include "storage/redis/shard.hpp"
#include "storage/redis/shard_router.hpp"
#include <memory>
#include <type_traits>

namespace run::redis
{

class ShardManager
{
public:
    ShardManager(boost::asio::any_io_executor& io_context, const SentinelConfig& config,
                 const boost::redis::request* setup_commands = nullptr)
        : m_shard_router{config.num_shards}
    {
        m_shards.reserve(config.num_shards);
        for (std::size_t i = 0; i < config.num_shards; ++i)
        {
            m_shards.push_back(std::make_shared<Shard>(io_context, config, i, setup_commands));
        }
    }

    template <typename Key, typename Request, typename Response>
        requires RoutingKey<std::remove_reference_t<Key>>
    boost::asio::awaitable<boost::system::error_code>
    Execute(Key&& routing_key, const Request& request, Response& response,
            std::chrono::milliseconds network_timeout = std::chrono::seconds(5))
    {
        std::size_t shard_index = m_shard_router.GetShardIndex(std::forward<Key>(routing_key));
        co_return co_await m_shards[shard_index]->AsyncExec(request, response, network_timeout);
    }

    std::shared_ptr<Shard> GetShard(std::size_t index)
    {
        if (index >= m_shards.size())
        {
            throw std::out_of_range("Invalid shard index");
        }

        return m_shards[index];
    }

    template <typename Key>
        requires RoutingKey<std::remove_reference_t<Key>>
    std::size_t GetShardIndex(Key&& key) const
    {
        return m_shard_router.GetShardIndex(std::forward<Key>(key));
    }

    std::size_t GetShardCount() const noexcept
    {
        return m_shards.size();
    }

private:
    ShardRouter m_shard_router;
    std::vector<std::shared_ptr<run::redis::Shard>> m_shards;
};

} // namespace run::redis