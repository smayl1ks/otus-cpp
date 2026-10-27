#pragma once

#include "storage/redis/redis_publisher.hpp"
#include "storage/redis/resilient_redis_producer.hpp"
#include "storage/redis/sentinel_config.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace run::storage::redis
{

namespace net = boost::asio;

class ShardedRedisManager : public std::enable_shared_from_this<ShardedRedisManager>
{
public:
    ShardedRedisManager(net::any_io_executor executor,
                        RedisConfig redis_cfg,
                        BufferConfig buffer_cfg);
    ~ShardedRedisManager() = default;

    ShardedRedisManager(const ShardedRedisManager&) = delete;
    ShardedRedisManager& operator=(const ShardedRedisManager&) = delete;

    net::awaitable<bool> InitializeAsync();
    void Stop();

    // Hot-path Telemetry Ingestion (sharded by run_id)
    net::awaitable<bool> PushTrackEventAsync(DeferredTrackEvent event);
    net::awaitable<bool> PushTrackBatchAsync(std::vector<DeferredTrackEvent> events);

    // WebSocket Ticket Management (sharded by ticket token)
    net::awaitable<bool> SetWsTicketAsync(const std::string& ticket, uint64_t user_id,
                                   std::chrono::seconds ttl = std::chrono::seconds(30));
    net::awaitable<std::optional<uint64_t>> ValidateWsTicketAsync(const std::string& ticket);

    // Pub/Sub Broadcast: dedicated radar fleet (client-side channel sharding by
    // h3_zone) when configured, single pub/sub instance, or legacy sharded
    // masters fallback
    net::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload);

    std::shared_ptr<RedisPublisher> GetPublisher(std::size_t index = 0) const noexcept;
    std::size_t GetPublisherCount() const noexcept;

    // Same routing rule as PublishAsync: channel -> publisher index
    std::size_t GetPublisherIndex(const std::string& channel) const noexcept;

    // Generic key-value helpers
    net::awaitable<bool> SetAsync(const std::string& key, const std::string& value, std::chrono::seconds ttl);
    net::awaitable<std::optional<bool>> SetNxAsync(const std::string& key, const std::string& value,
                                                   std::chrono::seconds ttl);
    net::awaitable<std::optional<std::string>> GetAsync(const std::string& key);
    net::awaitable<bool> DeleteAsync(const std::string& key);
    net::awaitable<bool> ExistsAsync(const std::string& key);

    // Generic Hash helpers
    net::awaitable<bool> HSetAsync(const std::string& key, const std::string& field, const std::string& value,
                             std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<bool> HSetMultipleAsync(const std::string& key,
                                           const std::vector<std::pair<std::string, std::string>>& fields,
                                           std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<bool> HSetMultipleAsync(const std::string& key,
                                           const std::vector<std::string>& flattened_fields,
                                           std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<std::optional<std::string>> HGetAsync(const std::string& key, const std::string& field);
    net::awaitable<std::unordered_map<std::string, std::string>> HGetAllAsync(const std::string& key);

    // List helpers (track retrieval)
    net::awaitable<std::vector<std::string>> LRangeAsync(const std::string& key, int64_t start, int64_t stop);
    net::awaitable<bool> RPushRangeAsync(const std::string& key, const std::vector<std::string>& values,
                                         std::optional<std::chrono::seconds> ttl = std::nullopt);

    // Set helpers (offline chunks tracking)
    net::awaitable<int64_t> SAddAsync(const std::string& key, const std::string& member,
                                      std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<std::vector<std::string>> SMembersAsync(const std::string& key);
    net::awaitable<int64_t> SCardAsync(const std::string& key);

    void InvalidateTtl(uint64_t run_id);

    // Shard routing queries
    std::size_t GetShardIndex(uint64_t key) const noexcept;
    std::size_t GetShardIndex(std::string_view key) const noexcept;

    std::shared_ptr<ResilientRedisProducer> GetShard(std::size_t index) const;
    std::size_t GetShardCount() const noexcept;

    // Monitoring & Observability
    std::vector<CircuitState> GetCircuitStates() const;
    std::size_t GetTotalDroppedEvents() const noexcept;

    // Fast 64-bit mixer (SplitMix64)
    static uint64_t Hash64(uint64_t x) noexcept
    {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    // FNV-1a 64-bit string hash
    static uint64_t HashString(std::string_view str) noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        for (const char c : str)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

private:
    net::any_io_executor m_executor;
    RedisConfig m_base_redis_config;
    BufferConfig m_buffer_config;

    std::vector<std::shared_ptr<ResilientRedisProducer>> m_shards;
    std::vector<std::shared_ptr<RedisPublisher>> m_publishers;
};

} // namespace run::storage::redis
