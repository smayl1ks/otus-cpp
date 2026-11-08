#include "controllers/map_controller.hpp"

#include "network/server/envelope_dispatcher.hpp"

#include <algorithm>

namespace run::controller
{

bool IsValidViewportBBox(const gamemap::SubscribeViewportRequest& request, const ViewportBBoxLimits& limits)
{
    const double sw_lat = request.south_west_lat();
    const double sw_lng = request.south_west_lng();
    const double ne_lat = request.north_east_lat();
    const double ne_lng = request.north_east_lng();

    return std::isfinite(sw_lat) && std::isfinite(sw_lng) && std::isfinite(ne_lat) && std::isfinite(ne_lng) &&
           std::abs(sw_lat) <= 90.0 && std::abs(ne_lat) <= 90.0 && std::abs(sw_lng) <= 180.0 &&
           std::abs(ne_lng) <= 180.0 && sw_lat <= ne_lat && sw_lng <= ne_lng &&
           (ne_lat - sw_lat) <= limits.max_lat_span_deg && (ne_lng - sw_lng) <= limits.max_lng_span_deg;
}

std::vector<uint64_t> NormalizeZones(const google::protobuf::RepeatedField<uint64_t>& requested,
                                     const std::size_t max_zones)
{
    std::vector<uint64_t> zones;
    zones.reserve(requested.size());
    for (const uint64_t zone : requested)
    {
        if (zone != 0)
        {
            zones.push_back(zone);
        }
    }
    std::sort(zones.begin(), zones.end());
    zones.erase(std::unique(zones.begin(), zones.end()), zones.end());
    if (zones.size() > max_zones)
    {
        zones.resize(max_zones);
    }
    return zones;
}

MapController::MapController(std::shared_ptr<service::running::RunningServices> services)
    : m_services(std::move(services))
{
}

net::awaitable<void> MapController::HandleSubscribeViewport(const run::proto::Envelope& envelope,
                                                            run::server::session::UserSession& session) const
{
    const auto& request = envelope.subscribe_viewport_request();

    std::vector<uint64_t> zones;
    if (!request.zone_h3().empty())
    {
        zones = NormalizeZones(request.zone_h3());
    }
    else if (m_services->h3_service && IsValidViewportBBox(request))
    {
        const auto cells = m_services->h3_service->GetCellsInBoundingBox(
            request.south_west_lat(), request.south_west_lng(), request.north_east_lat(), request.north_east_lng(), 9);
        zones = m_services->h3_service->GetParentZones(cells, 6);
    }

    session.UpdateSubscribedZones(zones);

    auto response = run::server::MakeResponseEnvelope(envelope);
    auto* viewport = response.mutable_subscribe_viewport_response();
    viewport->set_status(common::SYSTEM_STATUS_OK);

    if (request.need_snapshot() && m_services->hex_map_cache && !zones.empty())
    {
        for (auto& hex : co_await m_services->hex_map_cache->GetSnapshotAsync(zones))
        {
            auto* message = viewport->add_hexagons();
            message->set_h3_index(hex.h3_index);
            message->set_owner_user_id(hex.owner_user_id);
            message->set_owner_username(hex.owner_username);
            message->set_owner_color_hex(hex.owner_color_hex);
            message->set_score(hex.score);
        }
    }

    session.SendProtobuf(response);
}

} // namespace run::controller
