#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <prometheus/summary.h>
#include <prometheus/text_serializer.h>

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

    bool StartServer(const std::string& bind_address = "0.0.0.0:9100")
    {
        std::lock_guard lock(m_server_mutex);
        if (!m_exposer)
        {
            try
            {
                m_exposer = std::make_unique<prometheus::Exposer>(bind_address);
                m_exposer->RegisterCollectable(m_registry);
                return true;
            }
            catch (const std::exception&)
            {
                return false;
            }
        }
        return true;
    }

    void StopServer()
    {
        std::lock_guard lock(m_server_mutex);
        m_exposer.reset();
    }

    std::string Collect() const
    {
        prometheus::TextSerializer serializer;
        return serializer.Serialize(m_registry->Collect());
    }

    // --- Domain gameplay metrics ---
    void IncrementResolvedTracks()
    {
        m_resolved_tracks.Increment();
    }

    void AddCapturedCells(double count)
    {
        m_total_captured_cells.Increment(count);
    }

    void ObserveLatency(double latency_us)
    {
        m_latency_summary.Observe(latency_us);
    }

    // --- Network observability ---
    void SetActiveSockets(double count)
    {
        m_active_sockets.Set(count);
    }

    void IncrementConnectionsAccepted(bool is_tls)
    {
        m_connections_accepted_family.Add({{ "tls", is_tls ? "true" : "false" }}).Increment();
    }

    void IncrementHttpRequests(std::string_view method, std::string_view endpoint, unsigned int status_code)
    {
        m_http_requests_family.Add({
            { "method", std::string(method) },
            { "endpoint", std::string(endpoint) },
            { "status", std::to_string(status_code) }
        }).Increment();
    }

    void IncrementWsEnvelopes(const std::string& payload_type)
    {
        m_ws_envelopes_family.Add({{ "payload", payload_type }}).Increment();
    }

    void ObserveWsDispatchDuration(double duration_us)
    {
        m_ws_dispatch_latency.Observe(duration_us);
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

          m_connections_accepted_family(prometheus::BuildCounter()
                                            .Name("run_connections_accepted_total")
                                            .Help("Total incoming TCP/TLS network connections accepted")
                                            .Register(*m_registry)),

          m_http_requests_family(prometheus::BuildCounter()
                                     .Name("run_http_requests_total")
                                     .Help("Total HTTP requests handled")
                                     .Register(*m_registry)),

          m_ws_envelopes_family(prometheus::BuildCounter()
                                    .Name("run_ws_envelopes_total")
                                    .Help("Total WebSocket envelopes processed by dispatcher")
                                    .Register(*m_registry)),

          m_ws_dispatch_latency_family(prometheus::BuildSummary()
                                           .Name("run_ws_dispatch_duration_microseconds")
                                           .Help("WebSocket envelope dispatch latency in microseconds")
                                           .Register(*m_registry)),
          m_ws_dispatch_latency(m_ws_dispatch_latency_family.Add({}, prometheus::Summary::Quantiles{
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

    std::mutex m_server_mutex;
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

    prometheus::Family<prometheus::Counter>& m_connections_accepted_family;
    prometheus::Family<prometheus::Counter>& m_http_requests_family;
    prometheus::Family<prometheus::Counter>& m_ws_envelopes_family;
    prometheus::Family<prometheus::Summary>& m_ws_dispatch_latency_family;
    prometheus::Summary& m_ws_dispatch_latency;

    prometheus::Family<prometheus::Gauge>& m_redis_state_family;
    prometheus::Family<prometheus::Counter>& m_redis_dropped_family;
    prometheus::Family<prometheus::Counter>& m_redis_drained_family;
    prometheus::Family<prometheus::Counter>& m_redis_drain_failures_family;
    prometheus::Family<prometheus::Counter>& m_redis_failovers_family;
};

} // namespace run::monitoring
