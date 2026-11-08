#pragma once

#include "envelope.pb.h"
#include "gamemap.pb.h"
#include "network/server/user_session.hpp"
#include "service/running/running_services.hpp"

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace run::controller
{

/// Viewport bounding box size limits to prevent unbounded memory allocation
struct ViewportBBoxLimits
{
    double max_lat_span_deg{2.0};
    double max_lng_span_deg{4.0};
};

/// Maximum number of zones a session can subscribe to simultaneously
constexpr std::size_t kMaxSubscribedZones = 128;

/// Validates that the client bbox is finite, correctly ordered, and within span limits
bool IsValidViewportBBox(const gamemap::SubscribeViewportRequest& request, const ViewportBBoxLimits& limits = {});

/// Returns a sorted, deduplicated, zero-free zone list capped at max_zones
std::vector<uint64_t> NormalizeZones(const google::protobuf::RepeatedField<uint64_t>& requested,
                                     std::size_t max_zones = kMaxSubscribedZones);

/// Handles map browsing and viewport subscriptions (zone subscriptions and snapshots)
class MapController
{
public:
    explicit MapController(std::shared_ptr<service::running::RunningServices> services);

    net::awaitable<void> HandleSubscribeViewport(const run::proto::Envelope& envelope,
                                                 run::server::session::UserSession& session) const;

private:
    std::shared_ptr<service::running::RunningServices> m_services;
};

} // namespace run::controller
