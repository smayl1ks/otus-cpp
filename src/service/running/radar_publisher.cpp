#include "service/running/radar_publisher.hpp"

#include "logger/logger.hpp"

#include "gamemap.pb.h"

#include <boost/asio.hpp>

#include <fmt/format.h>

namespace run::service::running
{

RadarPublisher::RadarPublisher(std::shared_ptr<IPubSubPublisher> transport,
                               std::shared_ptr<gameplay::territory_conquest::H3Service> h3_service,
                               RadarPublisherConfig config)
    : m_transport(std::move(transport)), m_h3_service(std::move(h3_service)), m_config(config)
{
}

net::awaitable<void>
RadarPublisher::PublishIfNeededAsync(int64_t user_id, const repository::ActiveRunRecord& run,
                                     std::span<const gameplay::anticheat::TelemetryPoint> batch_points,
                                     int64_t now_ms) const
{
    if (batch_points.empty() || run.visibility == repository::Visibility::kPrivate)
    {
        co_return;
    }

    const auto& latest = batch_points.back();
    const int64_t latency_ms = now_ms - latest.RecordedAtMs();
    if (latency_ms > m_config.freshness_window_ms)
    {
        co_return;
    }

    const auto hex = m_h3_service->PointToH3(latest.coord.latitude, latest.coord.longitude, 9);
    if (!hex.has_value())
    {
        LOG_WARN("[radar] cannot resolve h3 zone for user {} point", user_id);
        co_return;
    }

    gamemap::RadarPing ping;
    ping.set_user_id(static_cast<uint64_t>(user_id));
    ping.set_team_id(static_cast<uint64_t>(run.team_id));
    ping.set_latitude(latest.coord.latitude);
    ping.set_longitude(latest.coord.longitude);
    ping.set_speed(latest.speed_ms.value_or(0.0f));
    ping.set_timestamp(latest.RecordedAtMs());

    const std::string channel =
        run.visibility == repository::Visibility::kPublic
            ? fmt::format("{}:zone:{:x}:radar:public", m_config.city_prefix, *hex)
            : fmt::format("{}:zone:{:x}:radar:team:{}", m_config.city_prefix, *hex, run.team_id);

    // Fire-and-forget, detached from the ack path
    auto transport = m_transport;
    net::co_spawn(
        co_await net::this_coro::executor,
        [transport, channel = std::move(channel), ping = std::move(ping)]() mutable -> net::awaitable<void>
        {
            std::string payload;
            if (!ping.SerializeToString(&payload))
            {
                co_return;
            }
            if (!(co_await transport->PublishAsync(channel, payload)))
            {
                LOG_WARN("[radar] publish failed, channel '{}'", channel);
            }
        },
        net::detached);
}

} // namespace run::service::running
