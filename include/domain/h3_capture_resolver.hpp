#pragma once

#include "domain/h3_service.hpp"
#include "location.pb.h"

#include <cstddef>
#include <vector>

namespace run::domain::h3
{

/**
 * @brief Configuration parameters for H3 spatial capture resolution.
 *
 * Focuses purely on spatial grid rules and loop geometry.
 * Telemetry validation, mock detection, kinematics, and anti-cheat are handled upstream by GpsAntiCheat.
 */
struct H3CaptureConfig
{
    /** @brief Minimum confirmed raw GPS points in an H3 cell for it to be qualified (RUN_ROLES.md: N >= 2). */
    std::size_t min_points_per_hex{2};

    /**
     * @brief Minimum distance in meters traveled inside an H3 cell.
     * Prevents false captures from GPS jitter when stationary or clipping corner boundaries.
     * Default is 0.0 for backward compatibility with pure point-count configurations.
     */
    double min_distance_per_hex_meters{0.0};

    /** @brief Flag enabling loop detection and enclosed territory capture. */
    bool enable_loop_capture{true};

    /** @brief Minimum perimeter distance along the track to consider a closed loop (meters). */
    double min_loop_perimeter_meters{300.0};

    /** @brief Minimum enclosed polygon area in square meters (filters out corner turns and out-and-back tracks). */
    double min_loop_area_m2{10000.0};

    /** @brief Maximum straight-line distance in meters between loop start and end points for closure. */
    double max_loop_closure_distance_meters{150.0};
};

/**
 * @brief Detailed breakdown of the capture resolution result.
 */
struct CaptureResult
{
    std::vector<H3IndexValue> claimed_hexes;  ///< All claimed hexes (boundary + enclosed, deduplicated)
    std::vector<H3IndexValue> boundary_hexes; ///< Hexes visited directly along the trajectory
    std::vector<H3IndexValue> enclosed_hexes; ///< Hexes captured inside completed loops
    double total_distance_meters{0.0};        ///< Total valid distance covered
};

/**
 * @brief Resolves territory capture over an H3 spatial grid from validated GPS tracks.
 *
 * Implements spatial domain rules defined in docs/RUN_ROLES.md (Section 2):
 * - Minimum point count and minimum distance thresholding per H3 cell (anti-corner clipping).
 * - Closed loop detection and polygon rasterization to capture enclosed territory.
 */
class H3CaptureResolver
{
public:
    using LocationPoints = google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>;

    explicit H3CaptureResolver(const H3Service& h3_service, H3CaptureConfig config = {}) noexcept;

    /**
     * @brief Resolves the list of captured H3 hexagons for the given sequence of location points.
     * @param points Sequence of GPS location points.
     * @return Sorted, deduplicated list of valid captured H3 cell indexes.
     */
    CaptureResult Resolve(const LocationPoints& points) const;

    const H3CaptureConfig& GetConfig() const noexcept
    {
        return m_config;
    }

private:
    struct ProcessedPoint
    {
        H3IndexValue hex{0};
        double lat{0.0};
        double lon{0.0};
        double cum_distance{0.0};
    };

    std::vector<ProcessedPoint> BuildTrack(const LocationPoints& points, double& out_total_distance) const;

    std::vector<H3IndexValue> FilterBoundaryHexes(const std::vector<ProcessedPoint>& track) const;

    void DetectAndFillLoops(const std::vector<ProcessedPoint>& track, std::vector<H3IndexValue>& out_enclosed) const;

    std::vector<H3IndexValue> FillLoopPolygon(const std::vector<ProcessedPoint>& track, std::size_t start_idx,
                                              std::size_t end_idx, double signed_area) const;

    static double CalculateSignedPolygonAreaM2(const std::vector<ProcessedPoint>& track, std::size_t start_idx,
                                               std::size_t end_idx) noexcept;

private:
    const H3Service& m_h3_service;
    H3CaptureConfig m_config;
};

} // namespace run::domain::h3