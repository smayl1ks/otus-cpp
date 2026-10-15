#pragma once

#include "gameplay/geo_point.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace run::gameplay::anticheat
{

/**
 * @brief Domain-specific anti-cheat verification status.
 */
enum class AnticheatStatus : uint8_t
{
    Ok = 0,
    BatchTooLarge = 1,
    GpsAccuracyLow = 2,
    InvalidTimestamps = 3,
    SpeedLimitExceeded = 4,
    TeleportationDetected = 5,
    MockDetected = 6
};

constexpr std::string_view ToString(AnticheatStatus status) noexcept
{
    switch (status)
    {
    case AnticheatStatus::Ok:
        return "Ok";
    case AnticheatStatus::BatchTooLarge:
        return "BatchTooLarge";
    case AnticheatStatus::GpsAccuracyLow:
        return "GpsAccuracyLow";
    case AnticheatStatus::InvalidTimestamps:
        return "InvalidTimestamps";
    case AnticheatStatus::SpeedLimitExceeded:
        return "SpeedLimitExceeded";
    case AnticheatStatus::TeleportationDetected:
        return "TeleportationDetected";
    case AnticheatStatus::MockDetected:
        return "MockDetected";
    }
    return "Unknown";
}

/**
 * @brief Telemetry point received from client device for kinematic & anticheat verification.
 */
struct TelemetryPoint
{
    GeoPoint coord{};                                    ///< latitude, longitude
    std::chrono::system_clock::time_point recorded_at{}; ///< Sample time
    float accuracy_meters{0.0f};                         ///< GPS accuracy radius in meters
    std::optional<float> speed_ms{std::nullopt};         ///< Speed in m/s
    std::optional<float> altitude_meters{std::nullopt};  ///< Altitude above sea level in meters
    std::optional<float> heading_degrees{std::nullopt};  ///< Bearing in degrees [0, 360)
    bool is_mock{false};                                 ///< Mock provider
    float accelerometer_z{0.0f};                         ///< Z-axis acceleration for cadence check

    int64_t RecordedAtMs() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(recorded_at.time_since_epoch()).count();
    }

    constexpr bool operator==(const TelemetryPoint&) const noexcept = default;

    static TelemetryPoint Create(double lat, double lon, int64_t timestamp_ms, float accuracy = 5.0f,
                                 bool is_mock = false, std::optional<float> speed = std::nullopt,
                                 std::optional<float> altitude = std::nullopt,
                                 std::optional<float> heading = std::nullopt, float accel_z = 0.0f) noexcept
    {
        TelemetryPoint pt;
        pt.coord = GeoPoint{.latitude = lat, .longitude = lon};
        pt.recorded_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{timestamp_ms}};
        pt.accuracy_meters = accuracy;
        pt.is_mock = is_mock;
        pt.speed_ms = speed;
        pt.altitude_meters = altitude;
        pt.heading_degrees = heading;
        pt.accelerometer_z = accel_z;
        return pt;
    }
};

/**
 * @brief Configuration parameters for telemetry anti-cheat protection.
 */
struct GpsAntiCheatConfig
{
    float max_gps_accuracy_meters{30.0f};     ///< Max GPS accuracy radius in meters
    double max_running_speed_kmh{35.0};       ///< Max running speed (35 km/h)
    double max_teleport_distance_m{100.0};    ///< Max instant jump distance in meters
    int64_t max_allowed_future_time_ms{5000}; ///< Max allowed clock skew from future (5s)
    std::size_t max_points_per_batch{100};    ///< Max points processed in single batch
};

/**
 * @brief Telemetry validator kinematics, timestamps, and spoofing.
 */
class GpsAntiCheat
{
public:
    explicit GpsAntiCheat(GpsAntiCheatConfig config = {}) noexcept : m_config(config)
    {
    }

    ~GpsAntiCheat() = default;

    AnticheatStatus ValidateBatch(std::span<const TelemetryPoint> points,
                                  int64_t current_server_time_ms) const noexcept;

    AnticheatStatus ValidatePoint(const TelemetryPoint& point, int64_t current_server_time_ms) const noexcept;

    AnticheatStatus ValidateSegment(const TelemetryPoint& prev_point, const TelemetryPoint& curr_point) const noexcept;

    const GpsAntiCheatConfig& GetConfig() const noexcept
    {
        return m_config;
    }

private:
    GpsAntiCheatConfig m_config;
};

} // namespace run::gameplay::anticheat
