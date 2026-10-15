#include "gameplay/territory_conquest/capture_resolver.hpp"
#include "gameplay/territory_conquest/polygon_geometry.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"

#include <algorithm>
#include <unordered_map>

namespace run::gameplay::territory_conquest
{

namespace
{
struct HexMetrics
{
    std::size_t point_count{0};
    double distance_meters{0.0};
};

void DeduplicateAndValidate(std::vector<H3IndexValue>& hexes)
{
    std::sort(hexes.begin(), hexes.end());
    hexes.erase(std::unique(hexes.begin(), hexes.end()), hexes.end());
    hexes.erase(std::remove_if(hexes.begin(), hexes.end(),
                               [](H3IndexValue cell) { return cell == 0 || !H3Service::IsValid(cell); }),
                hexes.end());
}
} // namespace

std::vector<H3IndexValue> BoundaryQualifier::QualifyBoundaryHexes(std::span<const TrackPoint> track,
                                                                  std::size_t min_points_per_hex,
                                                                  double min_distance_per_hex_meters)
{
    if (track.empty())
    {
        return {};
    }

    std::unordered_map<H3IndexValue, HexMetrics> hex_stats;
    hex_stats.reserve(track.size());

    std::vector<H3IndexValue> visit_order;
    visit_order.reserve(track.size());

    // Record initial point
    hex_stats[track.front().hex].point_count = 1;
    visit_order.push_back(track.front().hex);

    for (std::size_t i = 1; i < track.size(); ++i)
    {
        const auto& prev = track[i - 1];
        const auto& curr = track[i];

        auto& curr_entry = hex_stats[curr.hex];
        if (curr_entry.point_count == 0)
        {
            visit_order.push_back(curr.hex);
        }
        curr_entry.point_count++;

        const double seg_dist = H3Service::GetDistanceMeters(prev.coord.latitude, prev.coord.longitude,
                                                             curr.coord.latitude, curr.coord.longitude);

        if (prev.hex == curr.hex)
        {
            curr_entry.distance_meters += seg_dist;
        }
        else
        {
            // Boundary crossing: split distance 50/50 between exiting and entering cell
            hex_stats[prev.hex].distance_meters += seg_dist * 0.5;
            curr_entry.distance_meters += seg_dist * 0.5;
        }
    }

    std::vector<H3IndexValue> qualified;
    qualified.reserve(visit_order.size());

    for (const auto hex : visit_order)
    {
        const auto it = hex_stats.find(hex);
        if (it != hex_stats.end())
        {
            const auto& metrics = it->second;
            if (metrics.point_count >= min_points_per_hex && metrics.distance_meters >= min_distance_per_hex_meters)
            {
                qualified.push_back(hex);
            }
        }
    }

    return qualified;
}

std::vector<H3IndexValue> PolygonRasterizer::RasterizeLoop(const H3Service& h3_service,
                                                           std::span<const TrackPoint> track, const ClosedLoop& loop)
{
    if (loop.end_idx <= loop.start_idx + 2 || loop.end_idx >= track.size())
    {
        return {};
    }

    std::vector<GeoPoint> coords;
    coords.reserve(loop.end_idx - loop.start_idx + 1);
    for (std::size_t i = loop.start_idx; i <= loop.end_idx; ++i)
    {
        coords.push_back(track[i].coord);
    }

    auto loop_verts = PolygonGeometry::BuildNormalizedH3Loop(coords, loop.signed_area);
    if (loop_verts.size() < 3)
    {
        return {};
    }

    GeoLoop outer_loop;
    outer_loop.numVerts = static_cast<int>(loop_verts.size());
    outer_loop.verts = loop_verts.data();

    GeoPolygon polygon;
    polygon.geoloop = outer_loop;
    polygon.numHoles = 0;
    polygon.holes = nullptr;

    return h3_service.PolygonToHexagons(polygon);
}

H3CaptureResolver::H3CaptureResolver(const H3Service& h3_service, H3CaptureConfig config) noexcept
    : m_h3_service(h3_service), m_config(config),
      m_loop_detector(h3_service, LoopDetectorConfig{
                                      .enable_loop_capture = config.enable_loop_capture,
                                      .min_loop_perimeter_meters = config.min_loop_perimeter_meters,
                                      .min_loop_area_m2 = config.min_loop_area_m2,
                                      .max_loop_closure_distance_meters = config.max_loop_closure_distance_meters,
                                  })
{
}

template <typename PointAccessor>
std::vector<TrackPoint> H3CaptureResolver::BuildTrackFromAccessor(std::size_t point_count, PointAccessor&& get_point,
                                                                  double& out_total_distance) const
{
    out_total_distance = 0.0;
    if (point_count == 0)
    {
        return {};
    }

    std::vector<TrackPoint> track;
    track.reserve(point_count);

    for (std::size_t i = 0; i < point_count; ++i)
    {
        const auto [lat, lon] = get_point(i);
        const auto hex_opt = m_h3_service.PointToH3(lat, lon);
        if (!hex_opt.has_value())
        {
            continue;
        }

        double cum_dist = 0.0;
        if (!track.empty())
        {
            const auto& prev = track.back();
            const double seg_dist = H3Service::GetDistanceMeters(prev.coord.latitude, prev.coord.longitude, lat, lon);
            cum_dist = prev.cum_distance + seg_dist;
            out_total_distance += seg_dist;
        }

        track.push_back(TrackPoint{
            .hex = *hex_opt,
            .coord = GeoPoint{.latitude = lat, .longitude = lon},
            .cum_distance = cum_dist,
        });
    }

    return track;
}

CaptureResult H3CaptureResolver::Resolve(std::span<const GeoPoint> points) const
{
    if (points.empty())
    {
        return {};
    }

    double total_distance = 0.0;
    const auto track = BuildTrackFromAccessor(
        points.size(), [points](std::size_t idx) -> std::pair<double, double>
        { return {points[idx].latitude, points[idx].longitude}; }, total_distance);

    return ResolveTrack(track, total_distance);
}

CaptureResult H3CaptureResolver::Resolve(std::span<const workout_tracking::RunPoint> points) const
{
    if (points.empty())
    {
        return {};
    }

    double total_distance = 0.0;
    const auto track = BuildTrackFromAccessor(
        points.size(), [points](std::size_t idx) -> std::pair<double, double>
        { return {points[idx].coord.latitude, points[idx].coord.longitude}; }, total_distance);

    return ResolveTrack(track, total_distance);
}

CaptureResult H3CaptureResolver::Resolve(std::span<const anticheat::TelemetryPoint> points) const
{
    if (points.empty())
    {
        return {};
    }

    double total_distance = 0.0;
    const auto track = BuildTrackFromAccessor(
        points.size(), [points](std::size_t idx) -> std::pair<double, double>
        { return {points[idx].coord.latitude, points[idx].coord.longitude}; }, total_distance);

    return ResolveTrack(track, total_distance);
}

CaptureResult H3CaptureResolver::ResolveTrack(const std::vector<TrackPoint>& track, double total_distance) const
{
    if (track.empty())
    {
        return {};
    }

    CaptureResult result;
    result.total_distance_meters = total_distance;

    // 1. Qualify boundary hex visits (satisfying count and distance thresholds)
    result.boundary_hexes = BoundaryQualifier::QualifyBoundaryHexes(track, m_config.min_points_per_hex,
                                                                    m_config.min_distance_per_hex_meters);

    // 2. Closed loop detection and polygon rasterization
    if (m_config.enable_loop_capture)
    {
        const auto closed_loops = m_loop_detector.DetectLoops(track);
        for (const auto& loop : closed_loops)
        {
            auto filled = PolygonRasterizer::RasterizeLoop(m_h3_service, track, loop);
            result.enclosed_hexes.insert(result.enclosed_hexes.end(), filled.begin(), filled.end());
        }
        DeduplicateAndValidate(result.enclosed_hexes);
    }

    // 3. Aggregate boundary + enclosed into total claimed
    result.claimed_hexes.reserve(result.boundary_hexes.size() + result.enclosed_hexes.size());
    result.claimed_hexes.insert(result.claimed_hexes.end(), result.boundary_hexes.begin(), result.boundary_hexes.end());
    result.claimed_hexes.insert(result.claimed_hexes.end(), result.enclosed_hexes.begin(), result.enclosed_hexes.end());
    DeduplicateAndValidate(result.claimed_hexes);

    return result;
}

} // namespace run::gameplay::territory_conquest
