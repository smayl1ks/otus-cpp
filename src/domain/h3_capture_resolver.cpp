#include "domain/h3_capture_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <unordered_map>

namespace run::domain::h3
{

namespace
{

constexpr double g_EarthRadiusMeters = 6371000.0; // WGS84
constexpr double g_DegToRad = std::numbers::pi / 180.0;
constexpr double g_CoordEpsilon = 1e-9;
constexpr double g_MaxNeighborClosureMeters = 60.0;
constexpr double g_MinLoopRatio = 3.0;

} // namespace

H3CaptureResolver::H3CaptureResolver(const H3Service& h3_service, H3CaptureConfig config) noexcept
    : m_h3_service(h3_service), m_config(config)
{
}

CaptureResult H3CaptureResolver::Resolve(const LocationPoints& points) const
{
    CaptureResult result;
    if (points.empty())
    {
        return {};
    }

    const auto track = BuildTrack(points, result.total_distance_meters);
    if (track.empty())
    {
        return {};
    }

    result.boundary_hexes = FilterBoundaryHexes(track);

    if (m_config.enable_loop_capture)
    {
        DetectAndFillLoops(track, result.enclosed_hexes);
    }

    result.claimed_hexes.reserve(result.boundary_hexes.size() + result.enclosed_hexes.size());
    result.claimed_hexes.insert(result.claimed_hexes.end(), result.boundary_hexes.begin(), result.boundary_hexes.end());
    result.claimed_hexes.insert(result.claimed_hexes.end(), result.enclosed_hexes.begin(), result.enclosed_hexes.end());

    std::sort(result.claimed_hexes.begin(), result.claimed_hexes.end());
    result.claimed_hexes.erase(std::unique(result.claimed_hexes.begin(), result.claimed_hexes.end()),
                               result.claimed_hexes.end());

    result.claimed_hexes.erase(std::remove_if(result.claimed_hexes.begin(), result.claimed_hexes.end(),
                                              [](H3IndexValue cell) { return cell == 0 || !H3Service::IsValid(cell); }),
                               result.claimed_hexes.end());

    return result;
}

std::vector<H3CaptureResolver::ProcessedPoint> H3CaptureResolver::BuildTrack(const LocationPoints& points,
                                                                             double& out_total_distance) const
{
    out_total_distance = 0.0;
    if (points.empty())
    {
        return {};
    }

    std::vector<ProcessedPoint> track;
    track.reserve(points.size());

    for (int i = 0; i < points.size(); ++i)
    {
        const auto& p = points.Get(i);

        const auto hex_opt = m_h3_service.PointToH3(p.latitude(), p.longitude());
        if (!hex_opt.has_value() || !H3Service::IsValid(*hex_opt))
        {
            continue;
        }

        double cum_dist = 0.0;
        if (!track.empty())
        {
            const auto& prev = track.back();
            const double seg_dist = H3Service::GetDistanceMeters(prev.lat, prev.lon, p.latitude(), p.longitude());
            cum_dist = prev.cum_distance + seg_dist;
            out_total_distance += seg_dist;
        }

        track.push_back(
            ProcessedPoint{.hex = *hex_opt, .lat = p.latitude(), .lon = p.longitude(), .cum_distance = cum_dist});
    }

    return track;
}

std::vector<H3IndexValue> H3CaptureResolver::FilterBoundaryHexes(const std::vector<ProcessedPoint>& track) const
{
    if (track.empty())
    {
        return {};
    }

    std::unordered_map<H3IndexValue, std::size_t> point_counts;
    std::unordered_map<H3IndexValue, double> hex_distances;

    point_counts.reserve(track.size());
    hex_distances.reserve(track.size());

    point_counts[track.front().hex] = 1;

    for (std::size_t i = 1; i < track.size(); ++i)
    {
        const auto& prev = track[i - 1];
        const auto& curr = track[i];

        point_counts[curr.hex]++;

        const double seg_dist = H3Service::GetDistanceMeters(prev.lat, prev.lon, curr.lat, curr.lon);

        if (prev.hex == curr.hex)
        {
            hex_distances[curr.hex] += seg_dist;
        }
        else
        {
            // Boundary crossing: split distance between exiting and entering hex
            hex_distances[prev.hex] += seg_dist * 0.5;
            hex_distances[curr.hex] += seg_dist * 0.5;
        }
    }

    std::vector<H3IndexValue> qualified;
    qualified.reserve(point_counts.size());

    for (const auto& [hex, count] : point_counts)
    {
        if (count >= m_config.min_points_per_hex)
        {
            double dist = 0.0;
            if (auto it = hex_distances.find(hex); it != hex_distances.end())
            {
                dist = it->second;
            }

            if (dist >= m_config.min_distance_per_hex_meters)
            {
                qualified.push_back(hex);
            }
        }
    }

    return qualified;
}

double H3CaptureResolver::CalculateSignedPolygonAreaM2(const std::vector<ProcessedPoint>& track, std::size_t start_idx,
                                                       std::size_t end_idx) noexcept
{
    if (end_idx <= start_idx + 2)
    {
        return 0.0;
    }

    double sum_lat = 0.0;
    for (std::size_t i = start_idx; i <= end_idx; ++i)
    {
        sum_lat += track[i].lat;
    }
    const double mean_lat = sum_lat / static_cast<double>(end_idx - start_idx + 1);

    const double rad_lat = mean_lat * g_DegToRad;
    const double meters_per_deg_lat = g_DegToRad * g_EarthRadiusMeters;
    const double meters_per_deg_lon = meters_per_deg_lat * std::cos(rad_lat);

    const double ref_lat = track[start_idx].lat;
    const double ref_lon = track[start_idx].lon;

    double area = 0.0;
    const std::size_t count = end_idx - start_idx + 1;
    for (std::size_t k = 0; k < count; ++k)
    {
        const auto& p1 = track[start_idx + k];
        const auto& p2 = track[start_idx + ((k + 1) % count)];

        const double x1 = (p1.lon - ref_lon) * meters_per_deg_lon;
        const double y1 = (p1.lat - ref_lat) * meters_per_deg_lat;
        const double x2 = (p2.lon - ref_lon) * meters_per_deg_lon;
        const double y2 = (p2.lat - ref_lat) * meters_per_deg_lat;

        area += (x1 * y2 - x2 * y1);
    }

    return area * 0.5;
}

std::vector<H3IndexValue> H3CaptureResolver::FillLoopPolygon(const std::vector<ProcessedPoint>& track,
                                                             std::size_t start_idx, std::size_t end_idx,
                                                             double signed_area) const
{
    if (end_idx <= start_idx + 2)
    {
        return {};
    }

    std::vector<LatLng> loop_verts;
    loop_verts.reserve(end_idx - start_idx + 1);

    for (std::size_t i = start_idx; i <= end_idx; ++i)
    {
        LatLng ll{.lat = degsToRads(track[i].lat), .lng = degsToRads(track[i].lon)};

        if (!loop_verts.empty() && std::abs(loop_verts.back().lat - ll.lat) < g_CoordEpsilon &&
            std::abs(loop_verts.back().lng - ll.lng) < g_CoordEpsilon)
        {
            continue;
        }

        loop_verts.push_back(ll);
    }

    while (loop_verts.size() > 2 && std::abs(loop_verts.back().lat - loop_verts.front().lat) < g_CoordEpsilon &&
           std::abs(loop_verts.back().lng - loop_verts.front().lng) < g_CoordEpsilon)
    {
        loop_verts.pop_back();
    }

    if (loop_verts.size() < 3)
    {
        return {};
    }

    if (signed_area < 0.0)
    {
        std::reverse(loop_verts.begin(), loop_verts.end());
    }

    GeoLoop outer_loop;
    outer_loop.numVerts = static_cast<int>(loop_verts.size());
    outer_loop.verts = loop_verts.data();

    GeoPolygon polygon;
    polygon.geoloop = outer_loop;
    polygon.numHoles = 0;
    polygon.holes = nullptr;

    return m_h3_service.PolygonToHexagons(polygon);
}

void H3CaptureResolver::DetectAndFillLoops(const std::vector<ProcessedPoint>& track,
                                           std::vector<H3IndexValue>& out_enclosed) const
{
    if (!m_config.enable_loop_capture || track.size() < 6)
    {
        return;
    }

    std::unordered_map<H3IndexValue, std::vector<std::size_t>> hex_to_indices;
    hex_to_indices.reserve(track.size());

    std::size_t window_start = 0;

    for (std::size_t j = 0; j < track.size(); ++j)
    {
        const auto& pt_j = track[j];

        std::optional<std::size_t> best_start_idx;
        double best_signed_area = 0.0;

        auto test_candidate = [&](std::size_t cand_idx) -> bool
        {
            if (cand_idx < window_start || j <= cand_idx + 2)
            {
                return false;
            }

            const double path_dist = pt_j.cum_distance - track[cand_idx].cum_distance;
            if (path_dist < m_config.min_loop_perimeter_meters)
            {
                return false;
            }

            const double closure_dist =
                H3Service::GetDistanceMeters(track[cand_idx].lat, track[cand_idx].lon, pt_j.lat, pt_j.lon);
            if (closure_dist > m_config.max_loop_closure_distance_meters)
            {
                return false;
            }

            const bool is_same_hex = (pt_j.hex == track[cand_idx].hex);
            if (!is_same_hex &&
                (closure_dist > g_MaxNeighborClosureMeters || path_dist < g_MinLoopRatio * closure_dist))
            {
                return false;
            }

            const double signed_area = CalculateSignedPolygonAreaM2(track, cand_idx, j);
            if (std::abs(signed_area) < m_config.min_loop_area_m2)
            {
                return false;
            }

            best_start_idx = cand_idx;
            best_signed_area = signed_area;
            return true;
        };

        if (auto it = hex_to_indices.find(pt_j.hex); it != hex_to_indices.end())
        {
            for (const auto cand_idx : it->second)
            {
                if (test_candidate(cand_idx))
                {
                    break;
                }
            }
        }

        if (!best_start_idx.has_value())
        {
            const auto neighbors = m_h3_service.GetHexagonsInRadius(pt_j.hex, 1);
            for (const auto n_hex : neighbors)
            {
                if (n_hex == pt_j.hex)
                {
                    continue;
                }
                if (auto it = hex_to_indices.find(n_hex); it != hex_to_indices.end())
                {
                    for (const auto cand_idx : it->second)
                    {
                        if (test_candidate(cand_idx))
                        {
                            break;
                        }
                    }
                }
                if (best_start_idx.has_value())
                {
                    break;
                }
            }
        }

        if (best_start_idx.has_value())
        {
            auto filled = FillLoopPolygon(track, *best_start_idx, j, best_signed_area);
            out_enclosed.insert(out_enclosed.end(), filled.begin(), filled.end());
            window_start = j;
        }

        hex_to_indices[pt_j.hex].push_back(j);
    }
}

} // namespace run::domain::h3