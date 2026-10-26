#pragma once

// Proto (session.*) <-> domain (repository::*) mapping for run attributes.
// Keeps the persistence interfaces proto-free: conversion happens once,
// at the service boundary.

#include "repository/run_records.hpp"
#include "session.pb.h"

namespace run::network::mappers
{

inline repository::RunMode ToDomain(run::proto::session::RunMode mode) noexcept
{
    return mode == run::proto::session::RUN_MODE_OFFLINE ? repository::RunMode::kOffline : repository::RunMode::kOnline;
}

inline repository::Visibility ToDomain(run::proto::session::VisibilityScope visibility) noexcept
{
    switch (visibility)
    {
    case run::proto::session::VISIBILITY_TEAM_ONLY:
        return repository::Visibility::kTeamOnly;
    case run::proto::session::VISIBILITY_PUBLIC:
        return repository::Visibility::kPublic;
    case run::proto::session::VISIBILITY_PRIVATE:
        break;
    }
    return repository::Visibility::kPrivate;
}

inline run::proto::session::RunMode ToProto(repository::RunMode mode) noexcept
{
    return mode == repository::RunMode::kOffline ? run::proto::session::RUN_MODE_OFFLINE
                                                 : run::proto::session::RUN_MODE_ONLINE;
}

inline run::proto::session::VisibilityScope ToProto(repository::Visibility visibility) noexcept
{
    switch (visibility)
    {
    case repository::Visibility::kTeamOnly:
        return run::proto::session::VISIBILITY_TEAM_ONLY;
    case repository::Visibility::kPublic:
        return run::proto::session::VISIBILITY_PUBLIC;
    case repository::Visibility::kPrivate:
        break;
    }
    return run::proto::session::VISIBILITY_PRIVATE;
}

inline run::proto::session::RunValidationReason ToProto(repository::ValidationVerdict verdict) noexcept
{
    using R = run::proto::session::RunValidationReason;
    switch (verdict)
    {
    case repository::ValidationVerdict::kTooShortDistance:
        return R::VALIDATION_REASON_TOO_SHORT_DISTANCE;
    case repository::ValidationVerdict::kTooShortDuration:
        return R::VALIDATION_REASON_TOO_SHORT_DURATION;
    case repository::ValidationVerdict::kTooLongDistance:
        return R::VALIDATION_REASON_TOO_LONG_DISTANCE;
    case repository::ValidationVerdict::kTooLongDuration:
        return R::VALIDATION_REASON_TOO_LONG_DURATION;
    case repository::ValidationVerdict::kAnticheatRejected:
        return R::VALIDATION_REASON_ANTICHEAT_REJECTED;
    case repository::ValidationVerdict::kInsufficientValidPoints:
        return R::VALIDATION_REASON_INSUFFICIENT_VALID_POINTS;
    case repository::ValidationVerdict::kInvalidTimestamps:
        return R::VALIDATION_REASON_INVALID_TIMESTAMPS;
    case repository::ValidationVerdict::kValid:
        break;
    }
    return R::VALIDATION_REASON_NONE;
}

inline run::proto::session::ActiveRunSnapshot BuildSnapshot(const repository::ActiveRunRecord& record)
{
    run::proto::session::ActiveRunSnapshot snapshot;
    snapshot.set_session_id(record.session_id);
    snapshot.set_server_run_id(record.server_run_id);
    snapshot.set_mode(ToProto(record.mode));
    snapshot.set_started_at(record.started_at_ms);
    snapshot.set_total_distance_meters(record.total_distance_meters);
    snapshot.set_total_points_count(record.total_points_count);
    snapshot.set_last_sequence_number(record.last_sequence_number);
    return snapshot;
}

inline run::proto::session::RunSummary BuildSummary(const repository::StoredRunSummary& summary)
{
    run::proto::session::RunSummary proto;
    proto.set_session_id(summary.session_id);
    proto.set_server_run_id(summary.server_run_id);
    proto.set_status(summary.close_reason == repository::RunCloseReason::kCancelled
                         ? run::proto::session::RUN_STATUS_CANCELLED
                         : run::proto::session::RUN_STATUS_FINISHED);
    proto.set_total_distance_meters(summary.total_distance_meters);
    proto.set_duration_seconds(summary.duration_seconds);
    proto.set_moving_duration_seconds(summary.moving_duration_seconds);
    proto.set_avg_pace_sec_per_km(summary.avg_pace_sec_per_km);
    proto.set_avg_speed_kmh(summary.avg_speed_kmh);
    proto.set_total_points_count(summary.total_points_count);
    proto.set_hexagons_captured_count(summary.hexagons_captured_count);
    proto.set_total_score(summary.total_score);
    for (const uint64_t hex : summary.claimed_hexes)
    {
        proto.add_claimed_hexes(hex);
    }
    proto.set_is_valid(summary.is_valid);
    proto.set_validation_reason(ToProto(summary.verdict));
    proto.set_started_at_ms(summary.started_at_ms);
    proto.set_finished_at_ms(summary.finished_at_ms);
    return proto;
}

} // namespace run::network::mappers
