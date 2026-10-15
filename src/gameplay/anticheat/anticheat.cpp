#include "gameplay/anticheat/anticheat.hpp"

namespace run::gameplay::anticheat
{

namespace
{
constexpr int64_t g_MaxTeleportWindowMs = 1000; // 1s
constexpr double g_KmhToMs = 1000.0 / 3600.0;   // km/h to m/s
} // namespace

AnticheatStatus GpsAntiCheat::ValidatePoint(const TelemetryPoint& point, int64_t current_server_time_ms) const noexcept
{
    if (point.is_mock)
    {
        return AnticheatStatus::MockDetected;
    }

    if (point.accuracy_meters > m_config.max_gps_accuracy_meters)
    {
        return AnticheatStatus::GpsAccuracyLow;
    }

    if (point.RecordedAtMs() > current_server_time_ms + m_config.max_allowed_future_time_ms)
    {
        return AnticheatStatus::InvalidTimestamps;
    }

    return AnticheatStatus::Ok;
}

AnticheatStatus GpsAntiCheat::ValidateSegment(const TelemetryPoint& prev_point,
                                              const TelemetryPoint& curr_point) const noexcept
{
    const int64_t time_diff_ms = curr_point.RecordedAtMs() - prev_point.RecordedAtMs();
    if (time_diff_ms <= 0)
    {
        return AnticheatStatus::InvalidTimestamps;
    }

    const double distance_m = CalculateGCircleDistance(prev_point.coord.latitude, prev_point.coord.longitude,
                                                       curr_point.coord.latitude, curr_point.coord.longitude);

    // Оump validation (distance > threshold within <= 1s)
    if (distance_m > m_config.max_teleport_distance_m && time_diff_ms <= g_MaxTeleportWindowMs)
    {
        return AnticheatStatus::TeleportationDetected;
    }

    // Kinematic speed evaluation
    const double time_diff_sec = static_cast<double>(time_diff_ms) / 1000.0;
    const double speed_ms = distance_m / time_diff_sec;
    const double max_speed_ms = m_config.max_running_speed_kmh * g_KmhToMs;

    if (speed_ms > max_speed_ms)
    {
        return AnticheatStatus::SpeedLimitExceeded;
    }

    return AnticheatStatus::Ok;
}

AnticheatStatus GpsAntiCheat::ValidateBatch(std::span<const TelemetryPoint> points,
                                            int64_t current_server_time_ms) const noexcept
{
    if (points.empty())
    {
        return AnticheatStatus::Ok;
    }

    if (points.size() > m_config.max_points_per_batch)
    {
        return AnticheatStatus::BatchTooLarge;
    }

    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const auto& current_point = points[i];
        const auto point_status = ValidatePoint(current_point, current_server_time_ms);
        if (point_status != AnticheatStatus::Ok)
        {
            return point_status;
        }

        if (i > 0)
        {
            const auto& previous_point = points[i - 1];
            const auto segment_status = ValidateSegment(previous_point, current_point);
            if (segment_status != AnticheatStatus::Ok)
            {
                return segment_status;
            }
        }
    }

    return AnticheatStatus::Ok;
}

} // namespace run::gameplay::anticheat
