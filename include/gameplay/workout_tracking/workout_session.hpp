#pragma once

#include "gameplay/geo_point.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace run::gameplay::workout_tracking
{

/**
 * @brief Persistent telemetry point belonging to a run session.
 */
struct RunPoint
{
    int64_t run_id{0};      ///< Associated run ID (0 if not yet persisted)
    int32_t point_order{0}; ///< 0-based sequential point order in the workout track

    GeoPoint coord{};                                   ///< WGS84 coordinates (latitude, longitude in degrees)
    std::optional<float> altitude_meters{std::nullopt}; ///< Altitude above sea level in meters
    std::optional<float> speed_ms{std::nullopt};        ///< Instantaneous speed in meters per second
    float accuracy_meters{0.0f};                        ///< Horizontal accuracy estimate radius in meters
    std::optional<float> heading_degrees{std::nullopt}; ///< Direction of travel in degrees [0, 360)

    territory_conquest::H3IndexValue h3_index{0};    ///< Uber H3 spatial index (Resolution 9)
    std::optional<int32_t> step_count{std::nullopt}; ///< Cumulative pedometer steps at this point

    std::chrono::system_clock::time_point recorded_at{}; ///< Wall-clock timestamp when coordinate was sampled

    constexpr bool operator==(const RunPoint&) const noexcept = default;

    int64_t RecordedAtMs() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(recorded_at.time_since_epoch()).count();
    }

    static RunPoint Create(int64_t run_id, int32_t point_order, double lat, double lon, int64_t timestamp_ms,
                           territory_conquest::H3IndexValue h3_index = 0, float accuracy = 0.0f,
                           std::optional<float> speed = std::nullopt, std::optional<float> altitude = std::nullopt,
                           std::optional<int32_t> steps = std::nullopt) noexcept
    {
        RunPoint p;
        p.run_id = run_id;
        p.point_order = point_order;
        p.coord = GeoPoint{.latitude = lat, .longitude = lon};
        p.h3_index = h3_index;
        p.accuracy_meters = accuracy;
        p.speed_ms = speed;
        p.altitude_meters = altitude;
        p.step_count = steps;
        p.recorded_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{timestamp_ms}};
        return p;
    }
};

struct RunValidationConfig
{
    double min_distance_meters{100.0};
    int64_t min_duration_seconds{60};

    double max_distance_meters{150000.0}; // Max 150 km per run
    int64_t max_duration_seconds{43200};  // Max 12 hours per session
};

struct RunMetrics
{
    double total_distance_meters{0.0};
    int64_t duration_seconds{0};
    bool is_valid{false};
};

class RunValidator
{
public:
    explicit RunValidator(RunValidationConfig config = {}) : m_config(config)
    {
    }

    RunMetrics Validate(std::span<const RunPoint> points) const noexcept;
    RunMetrics Validate(std::span<const GeoPoint> points, int64_t duration_seconds) const noexcept;

    const RunValidationConfig& GetConfig() const noexcept
    {
        return m_config;
    }

private:
    bool CheckValidity(double total_distance, int64_t duration_sec) const noexcept
    {
        const bool is_distance_valid =
            (total_distance >= m_config.min_distance_meters) && (total_distance <= m_config.max_distance_meters);

        const bool is_duration_valid =
            (duration_sec >= m_config.min_duration_seconds) && (duration_sec <= m_config.max_duration_seconds);

        return is_distance_valid && is_duration_valid;
    }

private:
    RunValidationConfig m_config;
};

} // namespace run::gameplay::workout_tracking
