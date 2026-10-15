#include "gameplay/territory_conquest/loop_detector.hpp"
#include "gameplay/territory_conquest/polygon_geometry.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace run::gameplay::territory_conquest
{

namespace
{
constexpr std::size_t g_MinTrackSizeForLoop = 6;
constexpr double g_MaxNeighborClosureMeters = 60.0;
constexpr double g_MinLoopRatio = 3.0;
} // namespace

LoopDetector::LoopDetector(const H3Service& h3_service, LoopDetectorConfig config) noexcept
    : m_h3_service(h3_service), m_config(config)
{
}

bool LoopDetector::HasMinimumPerimeter(double perimeter) const noexcept
{
    return perimeter >= m_config.min_loop_perimeter_meters;
}

bool LoopDetector::IsWithinClosureDistance(double closure_dist) const noexcept
{
    return closure_dist <= m_config.max_loop_closure_distance_meters;
}

bool LoopDetector::IsValidClosureTopology(bool is_same_hex, double closure_dist, double perimeter) const noexcept
{
    if (is_same_hex)
    {
        return true;
    }

    // When closing in an adjacent 1-ring neighbor cell, require tight proximity
    // and prevent shuttle out-and-back tracks (perimeter must be >= 3x closure gap)
    return (closure_dist <= g_MaxNeighborClosureMeters) && (perimeter >= g_MinLoopRatio * closure_dist);
}

bool LoopDetector::EnclosesSufficientArea(double signed_area) const noexcept
{
    return std::abs(signed_area) >= m_config.min_loop_area_m2;
}

bool LoopDetector::CanFormLoop(std::span<const TrackPoint> track, std::size_t start_idx, std::size_t end_idx,
                               double& out_signed_area) const noexcept
{
    const auto& pt_start = track[start_idx];
    const auto& pt_end = track[end_idx];

    // 1. Check path perimeter along the track
    const double perimeter = pt_end.cum_distance - pt_start.cum_distance;
    if (!HasMinimumPerimeter(perimeter))
    {
        return false;
    }

    // 2. Check straight-line closure distance between start and end points
    const double closure_dist = H3Service::GetDistanceMeters(pt_start.coord.latitude, pt_start.coord.longitude,
                                                             pt_end.coord.latitude, pt_end.coord.longitude);
    if (!IsWithinClosureDistance(closure_dist))
    {
        return false;
    }

    // 3. Topology constraint: ensure closure is either in same hex or valid neighbor
    const bool is_same_hex = (pt_start.hex == pt_end.hex);
    if (!IsValidClosureTopology(is_same_hex, closure_dist, perimeter))
    {
        return false;
    }

    // 4. Calculate enclosed area and check against minimum threshold (filters straight street runs)
    std::vector<GeoPoint> polygon_coords;
    polygon_coords.reserve(end_idx - start_idx + 1);
    for (std::size_t i = start_idx; i <= end_idx; ++i)
    {
        polygon_coords.push_back(track[i].coord);
    }

    const double signed_area = PolygonGeometry::CalculateSignedAreaM2(polygon_coords);
    if (!EnclosesSufficientArea(signed_area))
    {
        return false;
    }

    out_signed_area = signed_area;
    return true;
}

std::vector<ClosedLoop> LoopDetector::DetectLoops(std::span<const TrackPoint> track) const
{
    if (!m_config.enable_loop_capture || track.size() < g_MinTrackSizeForLoop)
    {
        return {};
    }

    std::vector<ClosedLoop> closed_loops;
    std::unordered_map<H3IndexValue, std::vector<std::size_t>> hex_to_indices;
    hex_to_indices.reserve(track.size());

    std::size_t window_start = 0;

    for (std::size_t j = 0; j < track.size(); ++j)
    {
        const auto& pt_j = track[j];

        std::optional<std::size_t> best_start_idx;
        double best_signed_area = 0.0;

        auto TestCandidate = [&](std::size_t cand_idx) -> bool
        {
            if (cand_idx < window_start || j <= cand_idx + 2)
            {
                return false;
            }

            double signed_area = 0.0;
            if (CanFormLoop(track, cand_idx, j, signed_area))
            {
                best_start_idx = cand_idx;
                best_signed_area = signed_area;
                return true;
            }
            return false;
        };

        // 1. Check candidates with the exact same H3 cell
        if (auto it = hex_to_indices.find(pt_j.hex); it != hex_to_indices.end())
        {
            for (const auto cand_idx : it->second)
            {
                if (TestCandidate(cand_idx))
                {
                    break;
                }
            }
        }

        // 2. Check 1-ring neighbor cells
        if (!best_start_idx.has_value())
        {
            std::array<H3IndexValue, 7> neighbors{};
            if (gridDisk(pt_j.hex, 1, neighbors.data()) == E_SUCCESS)
            {
                for (const auto n_hex : neighbors)
                {
                    if (n_hex == H3_NULL || n_hex == pt_j.hex)
                    {
                        continue;
                    }

                    if (auto it = hex_to_indices.find(n_hex); it != hex_to_indices.end())
                    {
                        for (const auto cand_idx : it->second)
                        {
                            if (TestCandidate(cand_idx))
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
        }

        // 3. Register detected loop and slide window
        if (best_start_idx.has_value())
        {
            closed_loops.push_back(ClosedLoop{
                .start_idx = *best_start_idx,
                .end_idx = j,
                .signed_area = best_signed_area,
            });
            window_start = j;
        }

        hex_to_indices[pt_j.hex].push_back(j);
    }

    return closed_loops;
}

} // namespace run::gameplay::territory_conquest
