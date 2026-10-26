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

// Transport for live pings (Redis pub/sub in production, sharded by h3 zone
// inside the implementation).
struct IPubSubPublisher
{
    virtual ~IPubSubPublisher() = default;

    virtual boost::asio::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload) = 0;
};

/// Stands in for ShardedRedisManager::PublishAsync while the storage layer is stubbed.
class NullPubSubPublisher final : public IPubSubPublisher
{
public:
    boost::asio::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload) override;
};

/// Live runner visibility: publishes at most one RadarPing per ingested batch,
/// and only when the runner is actually online right now (fresh point,
/// non-private visibility). Older points of an offline-replay session are
/// filtered by the freshness window and never reach pub/sub.
struct RadarPublisherConfig
{
    int64_t freshness_window_ms{15'000};
    std::string_view city_prefix{"kazan"};
};

class RadarPublisher
{
public:
    RadarPublisher(std::shared_ptr<IPubSubPublisher> transport, std::shared_ptr<gameplay::territory_conquest::H3Service> h3_service,
                   RadarPublisherConfig config = {});

    /// Best-effort: failures are logged, never propagated to the ingest path.
    boost::asio::awaitable<void> PublishIfNeededAsync(int64_t user_id, const repository::ActiveRunRecord& run,
                                                      std::span<const gameplay::anticheat::TelemetryPoint> batch_points,
                                                      int64_t now_ms) const;

private:
    std::shared_ptr<IPubSubPublisher> m_transport;
    std::shared_ptr<gameplay::territory_conquest::H3Service> m_h3_service;
    RadarPublisherConfig m_config;
};

} // namespace run::service::running
