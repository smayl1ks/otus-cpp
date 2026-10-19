#pragma once

#include <memory>
#include <string>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <prometheus/summary.h>

namespace run::monitoring
{

class MetricsCollector
{
public:
    static MetricsCollector& Instance() noexcept
    {
        static MetricsCollector instance;
        return instance;
    }

    void StartServer(const std::string& bind_address = "0.0.0.0:9100")
    {
        m_exposer = std::make_unique<prometheus::Exposer>(bind_address);
        m_exposer->RegisterCollectable(m_registry);
    }

    void IncrementResolvedTracks()
    {
        m_resolved_tracks.Increment();
    }

    void AddCapturedCells(double count)
    {
        m_total_captured_cells.Increment(count);
    }

    void SetActiveSockets(double count)
    {
        m_active_sockets.Set(count);
    }

    void ObserveLatency(double latency_us)
    {
        m_latency_summary.Observe(latency_us);
    }

    // --- Redis & Circuit Breaker Observability ---
    void SetRedisCircuitState(std::size_t shard_id, double state)
    {
        m_redis_state_family.Add({{ "shard", std::to_string(shard_id) }}).Set(state);
    }

    void IncrementDroppedEvents(std::size_t shard_id, double count = 1.0)
    {
        m_redis_dropped_family.Add({{ "shard", std::to_string(shard_id) }}).Increment(count);
    }

    void IncrementDrainedEvents(std::size_t shard_id, double count = 1.0)
    {
        m_redis_drained_family.Add({{ "shard", std::to_string(shard_id) }}).Increment(count);
    }

    void IncrementDrainFailures(std::size_t shard_id, double count = 1.0)
    {
        m_redis_drain_failures_family.Add({{ "shard", std::to_string(shard_id) }}).Increment(count);
    }

    void IncrementSentinelFailover(const std::string& master_name)
    {
        m_redis_failovers_family.Add({{ "master", master_name }}).Increment();
    }

private:
    MetricsCollector()
        : m_registry(std::make_shared<prometheus::Registry>()),

          m_tracks_family(prometheus::BuildCounter()
                              .Name("run_resolved_tracks_total")
                              .Help("Total number of successfully resolved tracking paths")
                              .Register(*m_registry)),
          m_resolved_tracks(m_tracks_family.Add({})),

          m_cells_family(prometheus::BuildCounter()
                             .Name("run_captured_cells_total")
                             .Help("Total number of H3 cells claimed")
                             .Register(*m_registry)),
          m_total_captured_cells(m_cells_family.Add({})),

          m_sockets_family(prometheus::BuildGauge()
                               .Name("run_active_sockets")
                               .Help("Current number of active WebSocket connections")
                               .Register(*m_registry)),
          m_active_sockets(m_sockets_family.Add({})),

          m_latency_family(prometheus::BuildSummary()
                               .Name("run_resolve_latency_microseconds")
                               .Help("Resolution latency in microseconds")
                               .Register(*m_registry)),
          m_latency_summary(m_latency_family.Add({}, prometheus::Summary::Quantiles{
                                                         {0.5, 0.05}, {0.9, 0.01}, {0.99, 0.001}
                                                     })),

          m_redis_state_family(prometheus::BuildGauge()
                                   .Name("redis_circuit_state")
                                   .Help("Circuit breaker state: 0=Healthy, 1=Degraded, 2=Draining")
                                   .Register(*m_registry)),
          m_redis_dropped_family(prometheus::BuildCounter()
                                     .Name("redis_dropped_events_total")
                                     .Help("Total telemetry events dropped due to buffer overflow")
                                     .Register(*m_registry)),
          m_redis_drained_family(prometheus::BuildCounter()
                                     .Name("redis_drained_events_total")
                                     .Help("Total telemetry events drained back to Redis master")
                                     .Register(*m_registry)),
          m_redis_drain_failures_family(prometheus::BuildCounter()
                                            .Name("redis_drain_failures_total")
                                            .Help("Total telemetry events failed during drain")
                                            .Register(*m_registry)),
          m_redis_failovers_family(prometheus::BuildCounter()
                                       .Name("redis_sentinel_failovers_total")
                                       .Help("Total Sentinel master failover events")
                                       .Register(*m_registry))
    {
    }

    ~MetricsCollector() = default;

    std::unique_ptr<prometheus::Exposer> m_exposer;
    std::shared_ptr<prometheus::Registry> m_registry;

    prometheus::Family<prometheus::Counter>& m_tracks_family;
    prometheus::Counter& m_resolved_tracks;

    prometheus::Family<prometheus::Counter>& m_cells_family;
    prometheus::Counter& m_total_captured_cells;

    prometheus::Family<prometheus::Gauge>& m_sockets_family;
    prometheus::Gauge& m_active_sockets;

    prometheus::Family<prometheus::Summary>& m_latency_family;
    prometheus::Summary& m_latency_summary;

    prometheus::Family<prometheus::Gauge>& m_redis_state_family;
    prometheus::Family<prometheus::Counter>& m_redis_dropped_family;
    prometheus::Family<prometheus::Counter>& m_redis_drained_family;
    prometheus::Family<prometheus::Counter>& m_redis_drain_failures_family;
    prometheus::Family<prometheus::Counter>& m_redis_failovers_family;
};

} // namespace run::monitoring
