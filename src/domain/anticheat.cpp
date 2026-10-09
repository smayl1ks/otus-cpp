#include "domain/anticheat.hpp"

#include <cmath>
#include <numbers>

#include "common.pb.h"
#include "location.pb.h"

namespace run::domain::anticheat
{

namespace
{

constexpr int64_t g_MaxTeleportWindowMs = 1000;   // 1s
constexpr double g_EarthRadiusMeters = 6371000.0; // WGS84
constexpr double g_DegToRad = std::numbers::pi / 180.0;
constexpr double g_KmhToMs = 1000.0 / 3600.0; // km/h to m/s

} // namespace

double GpsAntiCheat::CalculateDistanceMeters(double lat1, double lon1, double lat2, double lon2) const noexcept
{
    const double d_lat = (lat2 - lat1) * g_DegToRad;
    const double d_lon = (lon2 - lon1) * g_DegToRad;

    const double a = std::sin(d_lat / 2.0) * std::sin(d_lat / 2.0) + std::cos(lat1 * g_DegToRad) *
                                                                         std::cos(lat2 * g_DegToRad) *
                                                                         std::sin(d_lon / 2.0) * std::sin(d_lon / 2.0);

    const double clamped_a = std::clamp(a, 0.0, 1.0); // if a > 1.0 -> sqrt(1 - a) -> NaN (for antipodal points)

    const double c = 2.0 * std::atan2(std::sqrt(clamped_a), std::sqrt(1.0 - clamped_a));
    return g_EarthRadiusMeters * c;
}

common::AnticheatStatus GpsAntiCheat::ValidatePoint(const run::proto::location::LocationPoint& point,
                                                    int64_t current_server_time_ms) const noexcept
{
    if (point.is_mock())
    {
        return common::ANTICHEAT_STATUS_MOCK_DETECTED;
    }

    if (point.accuracy() > m_config.max_gps_accuracy_meters)
    {
        return common::ANTICHEAT_STATUS_GPS_ACCURACY_LOW;
    }

    if (point.timestamp() > current_server_time_ms + m_config.max_allowed_future_time_ms)
    {
        return common::ANTICHEAT_STATUS_INVALID_TIMESTAMPS;
    }

    return common::ANTICHEAT_STATUS_OK;
}

common::AnticheatStatus
GpsAntiCheat::ValidateSegment(const run::proto::location::LocationPoint& prev_point,
                              const run::proto::location::LocationPoint& curr_point) const noexcept
{
    const int64_t time_diff_ms = curr_point.timestamp() - prev_point.timestamp();

    if (time_diff_ms <= 0)
    {
        return common::ANTICHEAT_STATUS_INVALID_TIMESTAMPS;
    }

    const double distance_m = CalculateDistanceMeters(prev_point.latitude(), prev_point.longitude(),
                                                      curr_point.latitude(), curr_point.longitude());

    // jump validation (<= 1s)
    if (distance_m > m_config.max_teleport_distance_m && time_diff_ms <= g_MaxTeleportWindowMs)
    {
        return common::ANTICHEAT_STATUS_TELEPORTATION_DETECTED;
    }

    // Kinematic speed evaluation
    const double time_diff_sec = static_cast<double>(time_diff_ms) / 1000.0;
    const double speed_ms = distance_m / time_diff_sec;
    const double max_speed_ms = m_config.max_running_speed_kmh * g_KmhToMs;

    if (speed_ms > max_speed_ms)
    {
        return common::ANTICHEAT_STATUS_SPEED_LIMIT_EXCEEDED;
    }

    return common::ANTICHEAT_STATUS_OK;
}

common::AnticheatStatus GpsAntiCheat::ValidateBatch(const run::proto::location::LocationBatch& batch,
                                                    int64_t current_server_time_ms) const noexcept
{
    const auto& points = batch.points();

    if (points.empty())
    {
        return common::ANTICHEAT_STATUS_OK;
    }

    if (static_cast<std::size_t>(points.size()) > m_config.max_points_per_batch)
    {
        return common::ANTICHEAT_STATUS_BATCH_TOO_LARGE;
    }

    for (int i = 0; i < points.size(); ++i)
    {
        const auto& current_point = points.Get(i);

        const auto point_status = ValidatePoint(current_point, current_server_time_ms);

        if (point_status != common::ANTICHEAT_STATUS_OK)
        {
            return point_status;
        }

        if (i > 0)
        {
            const auto& previous_point = points.Get(i - 1);

            const auto segment_status = ValidateSegment(previous_point, current_point);
            if (segment_status != common::ANTICHEAT_STATUS_OK)
            {
                return segment_status;
            }
        }
    }

    return common::ANTICHEAT_STATUS_OK;
}

} // namespace run::domain::anticheat