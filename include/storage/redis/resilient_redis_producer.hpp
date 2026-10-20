#pragma once

#include "storage/in_memory_lock_free_buffer.hpp"
#include "storage/redis/drain_worker.hpp"
#include "storage/redis/redis_connection_pool.hpp"
#include "storage/redis/sentinel_config.hpp"
#include "storage/redis/sentinel_monitor.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <memory>
#include <string>

namespace run::storage::redis
{

namespace net = boost::asio;

enum class CircuitState
{
    Healthy = 0,   // Redis is operational; direct write, fallback to RAM on error
    Degraded = 1,  // Redis is down; instant write to in-memory lock-free buffer
    Draining = 2   // Redis recovered; writing to buffer while background worker drains
};

inline const char* ToString(CircuitState state) noexcept
{
    switch (state)
    {
    case CircuitState::Healthy:
        return "Healthy";
    case CircuitState::Degraded:
        return "Degraded";
    case CircuitState::Draining:
        return "Draining";
    default:
        return "Unknown";
    }
}

class ResilientRedisProducer : public std::enable_shared_from_this<ResilientRedisProducer>
{
public:
    using BufferType = storage::InMemoryLockFreeBuffer<DeferredTrackEvent>;

    ResilientRedisProducer(net::any_io_executor executor,
                           RedisConfig redis_cfg,
                           BufferConfig buffer_cfg,
                           std::size_t shard_id = 0);
    ~ResilientRedisProducer() = default;

    ResilientRedisProducer(const ResilientRedisProducer&) = delete;
    ResilientRedisProducer& operator=(const ResilientRedisProducer&) = delete;

    net::awaitable<bool> InitializeAsync();
    void Stop();

    // Hot-path Telemetry Ingestion
    net::awaitable<bool> PushTrackEventAsync(DeferredTrackEvent event);
    net::awaitable<bool> PushTrackBatchAsync(std::vector<DeferredTrackEvent> events);

    // Direct access to underlying Redis Pool (e.g. for WS tickets, Pub/Sub)
    std::shared_ptr<RedisConnectionPool> GetRedisPool() const noexcept;
    std::shared_ptr<BufferType> GetBuffer() const noexcept;

    CircuitState GetCircuitState() const noexcept;
    std::size_t GetDroppedEventsCount() const noexcept;
    std::size_t GetShardId() const noexcept;

private:
    void OnMasterFailover(const std::string& new_host, const std::string& new_port);
    net::awaitable<void> HealthCheckLoopAsync();
    net::awaitable<void> TriggerDrainingAsync();

private:
    net::any_io_executor m_executor;
    RedisConfig m_redis_config;
    BufferConfig m_buffer_config;
    std::size_t m_shard_id{0};

    std::atomic<CircuitState> m_state{CircuitState::Degraded};
    std::atomic<uint32_t> m_consecutive_failures{0};
    static constexpr uint32_t m_failure_threshold{2};

    std::atomic<bool> m_is_running{false};
    std::atomic<std::size_t> m_dropped_events{0};

    std::shared_ptr<SentinelMonitor> m_sentinel_monitor;
    std::shared_ptr<RedisConnectionPool> m_redis_pool;
    std::shared_ptr<BufferType> m_buffer;
    std::shared_ptr<DrainWorker> m_drain_worker;
};

} // namespace run::storage::redis
