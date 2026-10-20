
#include "storage/redis/resilient_redis_producer.hpp"
#include "logger/logger.hpp"
#include "monitoring/metrics_collector.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace run::storage::redis
{

ResilientRedisProducer::ResilientRedisProducer(net::any_io_executor executor, RedisConfig redis_cfg,
                                               BufferConfig buffer_cfg, std::size_t shard_id)
    : m_executor(executor), m_redis_config(std::move(redis_cfg)), m_buffer_config(buffer_cfg), m_shard_id(shard_id),
      m_sentinel_monitor(std::make_shared<SentinelMonitor>(executor, m_redis_config)),
      m_redis_pool(std::make_shared<RedisConnectionPool>(executor, m_redis_config)),
      m_buffer(std::make_shared<BufferType>(buffer_cfg.in_memory_capacity)),
      m_drain_worker(std::make_shared<DrainWorker>(executor, m_buffer, m_redis_pool, buffer_cfg, shard_id))
{
    monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                  static_cast<double>(CircuitState::Degraded));
}

net::awaitable<bool> ResilientRedisProducer::InitializeAsync()
{
    LOG_INFO("Initializing ResilientRedisProducer (Sentinel master: '{}', RAM buffer capacity: {}, shard id: {})...",
             m_redis_config.master_name, m_buffer_config.in_memory_capacity, m_shard_id);

    m_is_running = true;

    // 1. Register failover listener from Sentinel
    m_sentinel_monitor->RegisterOnMasterChanged([this](const std::string& new_host, const std::string& new_port)
                                                { OnMasterFailover(new_host, new_port); });

    // 2. Discover active master
    const bool resolved = co_await m_sentinel_monitor->ResolveMasterAsync();
    if (resolved)
    {
        const auto [host, port] = m_sentinel_monitor->GetCurrentMaster();
        m_redis_pool->ConnectToMaster(host, port);
        m_state.store(CircuitState::Healthy);
        monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                      static_cast<double>(CircuitState::Healthy));
        LOG_INFO("ResilientRedisProducer: Connected to Redis Master {}:{} [State: Healthy]", host, port);
    }
    else
    {
        m_state.store(CircuitState::Degraded);
        monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                      static_cast<double>(CircuitState::Degraded));
        LOG_WARN("ResilientRedisProducer: Could not resolve master at startup. Starting in DEGRADED mode.");
    }

    // 3. Start Sentinel Pub/Sub monitor
    m_sentinel_monitor->StartMonitoring();

    // 4. Start background probe loop
    net::co_spawn(m_executor, HealthCheckLoopAsync(), net::detached);

    co_return true;
}

void ResilientRedisProducer::Stop()
{
    m_is_running = false;
    if (m_sentinel_monitor)
    {
        m_sentinel_monitor->Stop();
    }
}

net::awaitable<bool> ResilientRedisProducer::PushTrackEventAsync(DeferredTrackEvent event)
{
    const auto current_state = m_state.load();

    if (current_state == CircuitState::Healthy)
    {
        // Direct push to Redis
        bool write_ok = co_await m_redis_pool->PushTrackPointAsync(event.run_id, event.latitude, event.longitude,
                                                                   event.speed, event.timestamp);

        if (write_ok && (event.delta_meters > 0.0 || event.delta_points > 0))
        {
            write_ok =
                co_await m_redis_pool->UpdateActiveRunStatsAsync(event.run_id, event.delta_meters, event.delta_points);
        }

        if (write_ok)
        {
            m_consecutive_failures.store(0);
            co_return true;
        }

        // Write failed: trip circuit breaker counter
        const auto failures = ++m_consecutive_failures;
        LOG_WARN("Direct Redis write failed (failure count: {}). Falling back to in-memory buffer.", failures);

        if (failures >= m_failure_threshold)
        {
            m_state.store(CircuitState::Degraded);
            monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                          static_cast<double>(CircuitState::Degraded));
            LOG_CRITICAL(">>> Circuit Breaker TRIPPED to DEGRADED state! Redis Master is unreachable. <<<");
        }
    }

    // Degraded or Draining state (or fallback on error): write to Lock-Free Buffer
    const bool pushed = m_buffer->Push(event);

    if (!pushed)
    {
        m_dropped_events++;
        monitoring::MetricsCollector::Instance().IncrementDroppedEvents(m_shard_id);
        LOG_CRITICAL("InMemoryLockFreeBuffer OVERFLOW! Dropping telemetry event for run {} (total dropped: {})",
                     event.run_id, m_dropped_events.load());
        co_return false;
    }

    co_return true;
}

net::awaitable<bool> ResilientRedisProducer::PushTrackBatchAsync(std::vector<DeferredTrackEvent> events)
{
    if (events.empty())
    {
        co_return true;
    }

    const auto current_state = m_state.load();

    if (current_state == CircuitState::Healthy)
    {
        const bool write_ok = co_await m_redis_pool->FlushTrackBatchAsync(events);
        if (write_ok)
        {
            m_consecutive_failures.store(0);
            co_return true;
        }

        const auto failures = ++m_consecutive_failures;
        LOG_WARN("Direct Redis batch write failed (failure count: {}). Falling back to in-memory buffer.", failures);

        if (failures >= m_failure_threshold)
        {
            m_state.store(CircuitState::Degraded);
            monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                          static_cast<double>(CircuitState::Degraded));
            LOG_CRITICAL(">>> Circuit Breaker TRIPPED to DEGRADED state! Redis Master is unreachable. <<<");
        }
    }

    bool all_pushed = true;
    for (auto& ev : events)
    {
        const bool pushed = m_buffer->Push(std::move(ev));
        if (!pushed)
        {
            m_dropped_events++;
            monitoring::MetricsCollector::Instance().IncrementDroppedEvents(m_shard_id);
            all_pushed = false;
        }
    }

    if (!all_pushed)
    {
        LOG_CRITICAL("InMemoryLockFreeBuffer OVERFLOW during batch push (total dropped: {})",
                     m_dropped_events.load());
    }

    co_return all_pushed;
}

void ResilientRedisProducer::OnMasterFailover(const std::string& new_host, const std::string& new_port)
{
    LOG_INFO("Sentinel notified master failover to {}:{}. Re-targeting connection pool...", new_host, new_port);

    m_redis_pool->UpdateMaster(new_host, new_port);
    m_consecutive_failures.store(0);

    // Switch to draining and flush buffer
    net::co_spawn(m_executor, TriggerDrainingAsync(), net::detached);
}

net::awaitable<void> ResilientRedisProducer::TriggerDrainingAsync()
{
    m_state.store(CircuitState::Draining);
    monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                  static_cast<double>(CircuitState::Draining));
    LOG_INFO("Circuit Breaker transitioned to DRAINING state. Flushing in-memory buffer to new Master...");

    const bool drained = co_await m_drain_worker->DrainAllAsync();
    if (drained && m_buffer->IsEmpty())
    {
        m_state.store(CircuitState::Healthy);
        monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                      static_cast<double>(CircuitState::Healthy));
        LOG_INFO(">>> Circuit Breaker restored to HEALTHY state! All events successfully synced to Redis. <<<");
    }
    else
    {
        LOG_WARN("Drain incomplete or failed. Reverting to DEGRADED state.");
        m_state.store(CircuitState::Degraded);
        monitoring::MetricsCollector::Instance().SetRedisCircuitState(m_shard_id,
                                                                      static_cast<double>(CircuitState::Degraded));
    }
}

net::awaitable<void> ResilientRedisProducer::HealthCheckLoopAsync()
{
    while (m_is_running)
    {
        net::steady_timer timer(co_await net::this_coro::executor);
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(net::use_awaitable);

        if (!m_is_running)
        {
            break;
        }

        const auto state = m_state.load();

        if (state == CircuitState::Degraded)
        {
            // Probe Redis
            LOG_DEBUG("Degraded state probe: checking Redis Master connection...");
            const bool ping_ok = co_await m_redis_pool->PingAsync();

            if (ping_ok)
            {
                LOG_INFO("Redis Master is responding again! Transitioning to Draining state.");
                co_await TriggerDrainingAsync();
            }
            else
            {
                // Try to query Sentinel in case failover happened without pub/sub notice
                const bool resolved = co_await m_sentinel_monitor->ResolveMasterAsync();
                if (resolved)
                {
                    const auto [host, port] = m_sentinel_monitor->GetCurrentMaster();
                    const auto [active_host, active_port] = m_redis_pool->GetActiveMaster();

                    if (host != active_host || port != active_port)
                    {
                        LOG_INFO("Sentinel discovered new master {}:{} during probe. Updating pool.", host, port);
                        m_redis_pool->UpdateMaster(host, port);
                        co_await TriggerDrainingAsync();
                    }
                }
            }
        }
    }
}

std::shared_ptr<RedisConnectionPool> ResilientRedisProducer::GetRedisPool() const noexcept
{
    return m_redis_pool;
}

std::shared_ptr<ResilientRedisProducer::BufferType> ResilientRedisProducer::GetBuffer() const noexcept
{
    return m_buffer;
}

CircuitState ResilientRedisProducer::GetCircuitState() const noexcept
{
    return m_state.load();
}

std::size_t ResilientRedisProducer::GetDroppedEventsCount() const noexcept
{
    return m_dropped_events.load();
}

std::size_t ResilientRedisProducer::GetShardId() const noexcept
{
    return m_shard_id;
}

} // namespace run::storage::redis
