#pragma once

#include "gameplay/geo_point.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace run::gameplay::territory_conquest
{

/**
 * @brief Represents a GPS track point projected to an H3 cell with cumulative distance.
 */
struct TrackPoint
{
    H3IndexValue hex{0};
    GeoPoint coord{};
    double cum_distance{0.0};
};

/**
 * @brief Detected closed loop interval along a track.
 */
struct ClosedLoop
{
    std::size_t start_idx{0};
    std::size_t end_idx{0};
    double signed_area{0.0};
};

/**
 * @brief Configuration thresholds for closed loop detection.
 */
struct LoopDetectorConfig
{
    bool enable_loop_capture{true};
    double min_loop_perimeter_meters{300.0};
    double min_loop_area_m2{10000.0};
    double max_loop_closure_distance_meters{150.0};
};

/**
 * @brief Algorithmic detector for closed topological loops along a runner's trajectory.
 */
class LoopDetector
{
public:
    explicit LoopDetector(const H3Service& h3_service, LoopDetectorConfig config = {}) noexcept;

    std::vector<ClosedLoop> DetectLoops(std::span<const TrackPoint> track) const;

    const LoopDetectorConfig& GetConfig() const noexcept
    {
        return m_config;
    }

private:
    bool CanFormLoop(std::span<const TrackPoint> track, std::size_t start_idx, std::size_t end_idx,
                     double& out_signed_area) const noexcept;

    bool HasMinimumPerimeter(double perimeter) const noexcept;
    bool IsWithinClosureDistance(double closure_dist) const noexcept;
    bool IsValidClosureTopology(bool is_same_hex, double closure_dist, double perimeter) const noexcept;
    bool EnclosesSufficientArea(double signed_area) const noexcept;

private:
    const H3Service& m_h3_service;
    LoopDetectorConfig m_config;
};

} // namespace run::gameplay::territory_conquest
