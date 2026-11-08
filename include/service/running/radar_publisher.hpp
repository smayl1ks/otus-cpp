#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "repository/run_records.hpp"

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <span>
#include <string_view>

namespace run::service::running
{

namespace net = boost::asio;

/// Transport for live runner pings (sharded by H3 zone in implementation)
struct IPubSubPublisher
{
    virtual ~IPubSubPublisher() = default;

    virtual net::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload) = 0;
};

/// Configuration for live runner visibility on the radar
struct RadarPublisherConfig
{
    int64_t freshness_window_ms{15'000};
    std::string_view city_prefix{"kazan"};
};

/// Publishes live runner pings for active, online, non-private runs
class RadarPublisher
{
public:
    RadarPublisher(std::shared_ptr<IPubSubPublisher> transport,
                   std::shared_ptr<gameplay::territory_conquest::H3Service> h3_service,
                   RadarPublisherConfig config = {});

    /// Best-effort publish (failures are logged, never blocking the ingest path)
    net::awaitable<void> PublishIfNeededAsync(int64_t user_id, const repository::ActiveRunRecord& run,
                                              std::span<const gameplay::anticheat::TelemetryPoint> batch_points,
                                              int64_t now_ms) const;

private:
    std::shared_ptr<IPubSubPublisher> m_transport;
    std::shared_ptr<gameplay::territory_conquest::H3Service> m_h3_service;
    RadarPublisherConfig m_config;
};

} // namespace run::service::running
