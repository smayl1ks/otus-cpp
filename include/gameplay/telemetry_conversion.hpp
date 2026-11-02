#pragma once

// Conversion between the two domain point types: the persisted track point
// (RunPoint) and the anticheat/telemetry point (TelemetryPoint). One
// definition for the whole project — the finalizer used to keep a private
// copy.

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
