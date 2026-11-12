#pragma once

#include "envelope.pb.h"
#include "gamemap.pb.h"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "logger/logger.hpp"
#include "network/server/session_manager.hpp"
#include "network/server/user_session.hpp"

#include <charconv>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
        // Team radar targets confirmed members only; capture events and public
        // radar broadcast to every subscriber of the zone (no filter)
        std::function<bool(const session::UserSession&)> filter;
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

        // Team channel suffix carries the publisher's team id; the session side
        // holds the profile-resolved membership (never client-supplied)
        constexpr std::string_view kTeamChannelMark = ":radar:team:";
        if (const auto mark = channel.rfind(kTeamChannelMark); mark != std::string_view::npos)
        {
            const std::string_view team_view = channel.substr(mark + kTeamChannelMark.size());
            uint64_t team_id = 0;
            const auto [ptr, ec] = std::from_chars(team_view.data(), team_view.data() + team_view.size(), team_id);
            if (ec == std::errc{} && ptr == team_view.data() + team_view.size())
            {
                filter = [team_id](const session::UserSession& session)
                { return session.GetTeamId() == std::optional<uint64_t>(team_id); };
            }
        }

        if (const auto parent6 = m_h3_service->GetParent(hex9, 6); parent6.has_value())
        {
            m_sessions->BroadcastToZone(*parent6, serialized, std::move(filter));
        }
    }

private:
    std::shared_ptr<session::SessionManager> m_sessions;
    std::shared_ptr<gameplay::territory_conquest::H3Service> m_h3_service;
};

} // namespace run::server
