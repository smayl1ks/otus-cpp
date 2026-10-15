#include "gameplay/workout_tracking/workout_session.hpp"

#include <algorithm>

namespace run::gameplay::workout_tracking
{

RunMetrics RunValidator::Validate(std::span<const RunPoint> points) const noexcept
{
    RunMetrics metrics;
    if (points.size() < 2)
    {
        return metrics;
    }

    for (std::size_t i = 1; i < points.size(); ++i)
    {
        const auto& prev = points[i - 1];
        const auto& curr = points[i];
        metrics.total_distance_meters += territory_conquest::H3Service::GetDistanceMeters(
            prev.coord.latitude, prev.coord.longitude, curr.coord.latitude, curr.coord.longitude);
    }

    const int64_t start_time = points.front().RecordedAtMs();
    const int64_t end_time = points.back().RecordedAtMs();
    metrics.duration_seconds = std::max<int64_t>(0, (end_time - start_time) / 1000);

    metrics.is_valid = CheckValidity(metrics.total_distance_meters, metrics.duration_seconds);
    return metrics;
}

RunMetrics RunValidator::Validate(std::span<const GeoPoint> points, int64_t duration_seconds) const noexcept
{
    RunMetrics metrics;
    if (points.size() < 2)
    {
        return metrics;
    }

    for (std::size_t i = 1; i < points.size(); ++i)
    {
        const auto& prev = points[i - 1];
        const auto& curr = points[i];
        metrics.total_distance_meters += territory_conquest::H3Service::GetDistanceMeters(
            prev.latitude, prev.longitude, curr.latitude, curr.longitude);
    }

    metrics.duration_seconds = std::max<int64_t>(0, duration_seconds);
    metrics.is_valid = CheckValidity(metrics.total_distance_meters, metrics.duration_seconds);
    return metrics;
}

} // namespace run::gameplay::workout_tracking
