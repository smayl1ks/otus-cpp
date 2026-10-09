#pragma once

#include <cstdint>
#include <memory>

namespace run::proto::location
{
class LocationBatch;
class LocationPoint;
} // namespace run::proto::location

namespace common
{
enum AnticheatStatus : int;
} // namespace common

namespace run::domain::anticheat
{

/**
 * @brief Configuration parameters for anti-cheat.
 */
struct GpsAntiCheatConfig
{
    /** @brief Maximum allowable GPS accuracy radius in meters. */
    float max_gps_accuracy_meters = 30.0f;

    /** @brief Maximum running speed in km/h. */
    double max_running_speed_kmh = 35.0;

    /** @brief Maximum allowed distance in meters between adjacent points. */
    double max_teleport_distance_m = 100.0;

    /** @brief Maximum allowable future clock skew for client timestamps. */
    int64_t max_allowed_future_time_ms = 5000;

    /** @brief Maximum allowed number of GPS points per batch. */
    std::size_t max_points_per_batch = 500;
};

/**
 * @brief Telemetry validator designed for anti-cheat protection and GPS anomaly filtering.
 *
 * Performs a fast-path validation pipeline over incoming GPS point batches,
 * evaluating movement kinematics, timestamp consistency, and location spoofing flags.
 */
class GpsAntiCheat
{
public:
    /**
     * @brief Constructs a validator instance with the specified configuration.
     * @param config Filtering thresholds and operational constraints.
     */
    explicit GpsAntiCheat(GpsAntiCheatConfig config = {}) : m_config(config)
    {
    }

    ~GpsAntiCheat() = default;

    common::AnticheatStatus ValidateBatch(const run::proto::location::LocationBatch& batch,
                                          int64_t current_server_time_ms) const noexcept;

private:
    /**
     * @brief Calculates the distance between points using the Haversine formula.
     */
    double CalculateDistanceMeters(double lat1, double lon1, double lat2, double lon2) const noexcept;

    /**
     * @brief Validates static attributes of a single GPS point.
     */
    common::AnticheatStatus ValidatePoint(const run::proto::location::LocationPoint& point,
                                          int64_t current_server_time_ms) const noexcept;

    /**
     * @brief Validates kinematic parameters between two consecutive GPS measurements.
     */
    common::AnticheatStatus ValidateSegment(const run::proto::location::LocationPoint& prev,
                                            const run::proto::location::LocationPoint& curr) const noexcept;

private:
    GpsAntiCheatConfig m_config;
};

} // namespace run::domain::anticheat