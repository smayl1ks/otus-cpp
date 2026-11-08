#pragma once

#include "envelope.pb.h"
#include "gamemap.pb.h"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "logger/logger.hpp"
#include "network/server/session_manager.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace run::server
{

/// Delivery side of the radar: parses a zone-channel payload (a bare
/// RadarPing on ":radar:" channels, a ready-to-send Envelope elsewhere) and
/// broadcasts it to the sessions subscribed to the channel's res-6 zone.
/// Pure conversion + fan-out: owns no sockets, safe to call from any
/// connection strand (SendProtobuf/BroadcastToZone are thread-safe).
class RadarFanout
{
public:
    RadarFanout(std::shared_ptr<session::SessionManager> sessions,
                std::shared_ptr<gameplay::territory_conquest::H3Service> h3_service)
        : m_sessions(std::move(sessions)), m_h3_service(std::move(h3_service))
    {
    }

    void operator()(std::string_view channel, std::string_view payload) const
    {
        run::proto::Envelope envelope;
        if (channel.find(":radar:") != std::string_view::npos)
        {
            // Radar channels carry a bare RadarPing; wrap it for clients.
            gamemap::RadarPing ping;
            if (!ping.ParseFromArray(payload.data(), static_cast<int>(payload.size())))
            {
                return;
            }

            envelope.set_timestamp(ping.timestamp());
            envelope.set_protocol_version(1);
            *envelope.mutable_radar_ping() = ping;
        }
        else
        {
            // Every other zone channel (capture events) carries a
            // ready-to-send Envelope.
            if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size())) ||
                !envelope.has_hexagon_capture_event())
            {
                return;
            }
        }

        std::string serialized;
        if (!envelope.SerializeToString(&serialized))
        {
            return;
        }

        const uint64_t hex9 = gameplay::territory_conquest::ZoneChannelHelper::ExtractH3Zone(channel);
        if (hex9 == 0 || !m_h3_service)
        {
            return;
        }

        if (const auto parent6 = m_h3_service->GetParent(hex9, 6); parent6.has_value())
        {
            m_sessions->BroadcastToZone(*parent6, serialized);
        }
    }

private:
    std::shared_ptr<session::SessionManager> m_sessions;
    std::shared_ptr<gameplay::territory_conquest::H3Service> m_h3_service;
};

} // namespace run::server
