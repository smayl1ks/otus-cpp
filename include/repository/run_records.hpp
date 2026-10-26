#pragma once

#include "gameplay/workout_tracking/workout_session.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace run::repository
{

// Domain-level run attributes. Transport enums (proto VisibilityScope / RunMode)
// are mapped at the network boundary (network/mappers/session_mapper.hpp).

enum class RunMode
{
    kOnline,
    kOffline
};

enum class Visibility
{
    kPrivate,
    kTeamOnly,
    kPublic
};

enum class RunCloseReason
{
    kFinished,
    kCancelled
};

// Validation verdict mirroring proto RunValidationReason 1:1.
enum class ValidationVerdict
{
    kValid,
    kTooShortDistance,
    kTooShortDuration,
    kTooLongDistance,
    kTooLongDuration,
    kAnticheatRejected,
    kInsufficientValidPoints,
    kInvalidTimestamps
};

// Live session state (Redis plane: run:active:{session_id}).
struct ActiveRunRecord
{
    std::string session_id;
    int64_t server_run_id{0};
    int64_t user_id{0};
    int64_t team_id{0};
    RunMode mode{RunMode::kOnline};
    Visibility visibility{Visibility::kPrivate};
    int64_t started_at_ms{0};

    // Accumulated ingestor progress, updated on every accepted batch.
    double total_distance_meters{0.0};
    int32_t total_points_count{0};
    int64_t last_sequence_number{0};
};

// Immutable result of a closed run, kept in Redis for idempotent
// finish/commit retries (run:finished:{session_id}, short TTL).
struct StoredRunSummary
{
    std::string session_id;
    int64_t server_run_id{0};
    int64_t user_id{0};
    RunCloseReason close_reason{RunCloseReason::kFinished};

    double total_distance_meters{0.0};
    uint32_t duration_seconds{0};
    uint32_t moving_duration_seconds{0};
    uint32_t avg_pace_sec_per_km{0};
    float avg_speed_kmh{0.0f};
    int32_t total_points_count{0};

    uint32_t hexagons_captured_count{0};
    uint32_t total_score{0};
    std::vector<uint64_t> claimed_hexes;

    ValidationVerdict verdict{ValidationVerdict::kValid};
    bool is_valid{true};

    int64_t started_at_ms{0};
    int64_t finished_at_ms{0};
};

// Assembling state of an offline upload (Redis plane: run:upload:{session_id}).
struct UploadMetaRecord
{
    std::string session_id;
    int64_t server_run_id{0};
    int64_t user_id{0};
    int64_t team_id{0};
    Visibility visibility{Visibility::kPrivate};
    int64_t start_timestamp_ms{0};
    int64_t finish_timestamp_ms{0};
    int64_t event_id{0};
    std::string device_id;
    std::string app_version;
    int32_t total_chunks{0};
};

// New history row (PostgreSQL plane: runs, status='active'), created at START.
struct NewRunRecord
{
    std::string session_id;
    int64_t user_id{0};
    int64_t team_id{0};
    RunMode mode{RunMode::kOnline};
    Visibility visibility{Visibility::kPrivate};
    int64_t started_at_ms{0};
    int64_t event_id{0};
    std::string device_id;
    std::string app_version;
};

} // namespace run::repository
