#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"

namespace run::gameplay
{

inline anticheat::TelemetryPoint ToTelemetryPoint(const workout_tracking::RunPoint& point) noexcept
{
    return anticheat::TelemetryPoint::Create(point.coord.latitude, point.coord.longitude, point.RecordedAtMs(),
                                             point.accuracy_meters, /*is_mock=*/false, point.speed_ms,
                                             point.altitude_meters, point.heading_degrees);
}

} // namespace run::gameplay
