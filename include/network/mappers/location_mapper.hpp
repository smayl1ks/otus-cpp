#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/geo_point.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"

#include "telemetry.pb.h"

#include <span>
#include <vector>

namespace run::network::mappers
{

/**
 * @brief Adapter mapping between transport Protobuf messages and pure domain entities.
 *
 * Located in the transport/network layer to maintain domain isolation.
 */
class LocationMapper
{
public:
    static gameplay::anticheat::TelemetryPoint
    ToTelemetryPoint(const ::run::proto::telemetry::LocationPoint& proto) noexcept
    {
        gameplay::anticheat::TelemetryPoint pt;
        pt.coord = gameplay::GeoPoint{.latitude = proto.latitude(), .longitude = proto.longitude()};
        pt.is_mock = proto.is_mock();
        pt.recorded_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{proto.timestamp()}};

        // Explicit proto3 field presence: unset sensor value is not the same as zero
        if (proto.has_accuracy())
        {
            pt.accuracy_meters = proto.accuracy();
        }
        if (proto.has_accelerometer_z())
        {
            pt.accelerometer_z = proto.accelerometer_z();
        }
        if (proto.has_speed())
        {
            pt.speed_ms = proto.speed();
        }
        if (proto.has_altitude())
        {
            pt.altitude_meters = static_cast<float>(proto.altitude());
        }
        if (proto.has_heading())
        {
            pt.heading_degrees = proto.heading();
        }
        return pt;
    }

    static std::vector<gameplay::anticheat::TelemetryPoint>
    ToTelemetryPoints(const ::run::proto::telemetry::LocationBatch& batch)
    {
        std::vector<gameplay::anticheat::TelemetryPoint> result;
        result.reserve(batch.points_size());
        for (const auto& proto_pt : batch.points())
        {
            result.push_back(ToTelemetryPoint(proto_pt));
        }
        return result;
    }

    static gameplay::workout_tracking::RunPoint
    ToRunPoint(const ::run::proto::telemetry::LocationPoint& proto, int64_t run_id = 0, int32_t point_order = 0,
               gameplay::territory_conquest::H3IndexValue h3_index = 0) noexcept
    {
        gameplay::workout_tracking::RunPoint p;
        p.run_id = run_id;
        p.point_order = point_order;
        p.coord = gameplay::GeoPoint{.latitude = proto.latitude(), .longitude = proto.longitude()};
        p.h3_index = h3_index;
        if (proto.has_accuracy())
        {
            p.accuracy_meters = proto.accuracy();
        }
        if (proto.has_altitude())
        {
            p.altitude_meters = static_cast<float>(proto.altitude());
        }
        if (proto.has_speed())
        {
            p.speed_ms = proto.speed();
        }
        if (proto.has_heading())
        {
            p.heading_degrees = proto.heading();
        }

        p.recorded_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{proto.timestamp()}};
        return p;
    }

    /// Batch of validated domain points -> track points with running order.
    static std::vector<gameplay::workout_tracking::RunPoint>
    ToRunPoints(int64_t server_run_id, std::span<const gameplay::anticheat::TelemetryPoint> points,
                int32_t first_point_order = 0)
    {
        std::vector<gameplay::workout_tracking::RunPoint> track;
        track.reserve(points.size());
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            auto& p = track.emplace_back();
            p.run_id = server_run_id;
            p.point_order = first_point_order + static_cast<int32_t>(i);
            p.coord = points[i].coord;
            p.accuracy_meters = points[i].accuracy_meters;
            p.speed_ms = points[i].speed_ms;
            p.altitude_meters = points[i].altitude_meters;
            p.heading_degrees = points[i].heading_degrees;
            p.recorded_at = points[i].recorded_at;
        }
        return track;
    }

    static void ToProtobuf(const gameplay::workout_tracking::RunPoint& entity,
                           ::run::proto::telemetry::LocationPoint& proto) noexcept
    {
        proto.set_latitude(entity.coord.latitude);
        proto.set_longitude(entity.coord.longitude);
        proto.set_timestamp(entity.RecordedAtMs());

        if (entity.accuracy_meters > 0.0f)
        {
            proto.set_accuracy(entity.accuracy_meters);
        }

        if (entity.altitude_meters.has_value())
        {
            proto.set_altitude(*entity.altitude_meters);
        }
        if (entity.speed_ms.has_value())
        {
            proto.set_speed(*entity.speed_ms);
        }
        if (entity.heading_degrees.has_value())
        {
            proto.set_heading(*entity.heading_degrees);
        }
    }

    static ::run::proto::telemetry::AnticheatStatus ToProto(gameplay::anticheat::AnticheatStatus status) noexcept
    {
        switch (status)
        {
        case gameplay::anticheat::AnticheatStatus::Ok:
            return ::run::proto::telemetry::ANTICHEAT_STATUS_OK;
        case gameplay::anticheat::AnticheatStatus::BatchTooLarge:
            return ::run::proto::telemetry::ANTICHEAT_STATUS_BATCH_TOO_LARGE;
        case gameplay::anticheat::AnticheatStatus::GpsAccuracyLow:
            return ::run::proto::telemetry::ANTICHEAT_STATUS_GPS_ACCURACY_LOW;
        case gameplay::anticheat::AnticheatStatus::InvalidTimestamps:
            return ::run::proto::telemetry::ANTICHEAT_STATUS_INVALID_TIMESTAMPS;
        case gameplay::anticheat::AnticheatStatus::SpeedLimitExceeded:
            return ::run::proto::telemetry::ANTICHEAT_STATUS_SPEED_LIMIT_EXCEEDED;
        case gameplay::anticheat::AnticheatStatus::TeleportationDetected:
            return ::run::proto::telemetry::ANTICHEAT_STATUS_TELEPORTATION_DETECTED;
        case gameplay::anticheat::AnticheatStatus::MockDetected:
            return ::run::proto::telemetry::ANTICHEAT_STATUS_MOCK_DETECTED;
        }
        return ::run::proto::telemetry::ANTICHEAT_STATUS_UNSPECIFIED;
    }

    static gameplay::anticheat::AnticheatStatus FromProto(::run::proto::telemetry::AnticheatStatus proto_status) noexcept
    {
        switch (proto_status)
        {
        case ::run::proto::telemetry::ANTICHEAT_STATUS_OK:
            return gameplay::anticheat::AnticheatStatus::Ok;
        case ::run::proto::telemetry::ANTICHEAT_STATUS_BATCH_TOO_LARGE:
            return gameplay::anticheat::AnticheatStatus::BatchTooLarge;
        case ::run::proto::telemetry::ANTICHEAT_STATUS_GPS_ACCURACY_LOW:
            return gameplay::anticheat::AnticheatStatus::GpsAccuracyLow;
        case ::run::proto::telemetry::ANTICHEAT_STATUS_INVALID_TIMESTAMPS:
            return gameplay::anticheat::AnticheatStatus::InvalidTimestamps;
        case ::run::proto::telemetry::ANTICHEAT_STATUS_SPEED_LIMIT_EXCEEDED:
            return gameplay::anticheat::AnticheatStatus::SpeedLimitExceeded;
        case ::run::proto::telemetry::ANTICHEAT_STATUS_TELEPORTATION_DETECTED:
            return gameplay::anticheat::AnticheatStatus::TeleportationDetected;
        case ::run::proto::telemetry::ANTICHEAT_STATUS_MOCK_DETECTED:
            return gameplay::anticheat::AnticheatStatus::MockDetected;
        default:
            return gameplay::anticheat::AnticheatStatus::Ok;
        }
    }
};

} // namespace run::network::mappers
