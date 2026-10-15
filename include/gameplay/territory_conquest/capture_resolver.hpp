#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/geo_point.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "gameplay/territory_conquest/loop_detector.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace run::gameplay::workout_tracking
{
struct RunPoint;
}

namespace run::gameplay::territory_conquest
{

/**
 * @brief Evaluates boundary H3 cell visits along a GPS trajectory according to docs/RUN_ROLES.md:
 *        - Minimum point count per cell (N >= min_points_per_hex)
 *        - Minimum distance traveled inside cell (D >= min_distance_per_hex_meters)
 *        - Boundary crossings split segment distance 50/50 between exiting and entering cells.
 *        - Results preserve the exact chronological order of first cell appearance.
 */
class BoundaryQualifier
{
public:
    static std::vector<H3IndexValue> QualifyBoundaryHexes(std::span<const TrackPoint> track,
                                                          std::size_t min_points_per_hex,
                                                          double min_distance_per_hex_meters);
};

/**
 * @brief Converts detected closed loops into a list of enclosed H3 hexagons.
 */
class PolygonRasterizer
{
public:
    static std::vector<H3IndexValue> RasterizeLoop(const H3Service& h3_service, std::span<const TrackPoint> track,
                                                   const ClosedLoop& loop);
};

/**
 * @brief Configuration parameters for H3 spatial capture resolution.
 */
struct H3CaptureConfig
{
    std::size_t min_points_per_hex{2};
    double min_distance_per_hex_meters{0.0};
    bool enable_loop_capture{true};
    double min_loop_perimeter_meters{300.0};
    double min_loop_area_m2{10000.0};
    double max_loop_closure_distance_meters{150.0};
};

/**
 * @brief Detailed breakdown of the capture resolution result.
 */
struct CaptureResult
{
    std::vector<H3IndexValue> claimed_hexes;  ///< All claimed hexes (boundary + enclosed, deduplicated, sorted)
    std::vector<H3IndexValue> boundary_hexes; ///< Hexes visited directly along trajectory (chronological)
    std::vector<H3IndexValue> enclosed_hexes; ///< Hexes captured inside completed loops (deduplicated, sorted)
    double total_distance_meters{0.0};        ///< Total valid distance covered
};

/**
 * @brief Coordinator resolving territory capture over an H3 spatial grid from GPS tracks.
 */
class H3CaptureResolver
{
public:
    explicit H3CaptureResolver(const H3Service& h3_service, H3CaptureConfig config = {}) noexcept;

    CaptureResult Resolve(std::span<const GeoPoint> points) const;
    CaptureResult Resolve(std::span<const workout_tracking::RunPoint> points) const;
    CaptureResult Resolve(std::span<const anticheat::TelemetryPoint> points) const;

    const H3CaptureConfig& GetConfig() const noexcept
    {
        return m_config;
    }

private:
    template <typename PointAccessor>
    std::vector<TrackPoint> BuildTrackFromAccessor(std::size_t point_count, PointAccessor&& get_point,
                                                   double& out_total_distance) const;

    CaptureResult ResolveTrack(const std::vector<TrackPoint>& track, double total_distance) const;

private:
    const H3Service& m_h3_service;
    H3CaptureConfig m_config;
    LoopDetector m_loop_detector;
};

} // namespace run::gameplay::territory_conquest
