#pragma once

#include <memory>
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
                                                         {0.5, 0.05}, {0.9, 0.01}, {0.99, 0.001} // p50, p90, p99
                                                     }))
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
};

} // namespace run::monitoring