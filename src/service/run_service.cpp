#include "service/run_service.hpp"
#include "common.pb.h"
#include "telemetry.pb.h"
#include "network/mappers/location_mapper.hpp"

// Логика работы сервиса — стадии и контракт: docs/RUN_LIFECYCLE.md
// 1. Старт: StartRunRequest (session_id, team_id, visibility, mode, start_timestamp,
//    event_id, device_id, app_version). Проверяем, нет ли уже активной пробежки.
//    Тот же session_id = resume (resumed=true + snapshot).
// 2. Поток: telemetry.LocationBatch -> telemetry.BatchProcessResponse (anti-cheat, прогресс).
// 3. Offline: пробежка без сети доигрывается батчами при reconnect (mode=OFFLINE)
//    или загружается после финиша через OfflineRunInit/Chunk/Commit.
// 4. Финиш: FinishRunRequest -> захват H3 одним проходом по всему треку -> RunSummary.

namespace run::service
{

RunService::RunService(std::shared_ptr<storage::redis::ShardedRedisManager> redis_manager,
                       std::shared_ptr<repository::ITrackRepository> track_repository,
                       gameplay::territory_conquest::H3CaptureConfig h3_config,
                       gameplay::anticheat::GpsAntiCheatConfig anticheat_config,
                       gameplay::workout_tracking::RunValidationConfig validation_config)
    : m_track_repository(std::move(track_repository)), m_redis(std::move(redis_manager)), m_anticheat(anticheat_config),
      m_run_validator(validation_config),
      m_capture_resolver(std::make_unique<gameplay::territory_conquest::H3CaptureResolver>(m_h3_service, h3_config))
{
}

Task<run::proto::session::StartRunResponse> RunService::StartRunAsync(int64_t user_id, const run::proto::session::StartRunRequest& request)
{
    run::proto::session::StartRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(run::proto::session::START_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());

    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    const bool active_exists = co_await m_track_repository->CheckActiveSessionExistsAsync(user_id);
    if (active_exists)
    {
        response.set_reject_reason(run::proto::session::START_REJECT_REASON_ACTIVE_SESSION_EXISTS);
        response.set_system_status(common::SYSTEM_STATUS_OK);
        co_return response;
    }

    const int64_t server_start_timestamp = GetCurrentTimestampMs();
    const bool inited = co_await m_track_repository->InitActiveRunAsync(request.session_id(), user_id,
                                                                        request.team_id(), server_start_timestamp);

    if (!inited)
    {
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    response.set_server_start_timestamp(server_start_timestamp);
    co_return response;
}

Task<run::proto::telemetry::BatchProcessResponse> RunService::ProcessBatchAsync(int64_t user_id,
                                                                 const run::proto::telemetry::LocationBatch& batch)
{
    run::proto::telemetry::BatchProcessResponse response;

    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_anticheat_status(run::proto::telemetry::ANTICHEAT_STATUS_OK);
    response.set_session_id(batch.session_id());
    response.set_batch_uuid(batch.batch_uuid());
    response.set_sequence_number(batch.sequence_number());

    if (batch.batch_uuid().empty() || batch.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    const bool session_exists = co_await m_track_repository->CheckActiveSessionExistsAsync(user_id);
    if (!session_exists)
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    if (batch.state() == run::proto::telemetry::RUNNING_STATE_PAUSED)
    {
        co_return response;
    }

    const auto game_points = network::mappers::LocationMapper::ToTelemetryPoints(batch);
    const auto anticheat_result = m_anticheat.ValidateBatch(game_points, GetCurrentTimestampMs());
    if (anticheat_result != gameplay::anticheat::AnticheatStatus::Ok)
    {
        response.set_anticheat_status(network::mappers::LocationMapper::ToProto(anticheat_result));
        co_return response;
    }

    const auto run_id = ParseRunId(batch.session_id());
    if (!run_id)
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    // Idempotency: client retries the same batch_uuid after network failures.
    // NX-claim is atomic; an existing key means this batch was already counted.
    const std::string dedup_key = fmt::format("run:batch:{}:{}", *run_id, batch.batch_uuid());
    auto dedup = co_await m_redis->SetNxAsync(dedup_key, "1", kBatchDedupTtl);
    if (dedup.has_value() && !*dedup)
    {
        co_return response; // duplicate: same OK ack as the original batch
    }

    // Territory conquest is intentionally NOT resolved here: captures are
    // computed once at finish over the whole track
    double total_delta_meters = 0.0;
    for (std::size_t i = 1; i < game_points.size(); ++i)
    {
        total_delta_meters += gameplay::territory_conquest::H3Service::GetDistanceMeters(
            game_points[i - 1].coord.latitude, game_points[i - 1].coord.longitude, game_points[i].coord.latitude,
            game_points[i].coord.longitude);
    }
    const int32_t delta_points = static_cast<int32_t>(game_points.size());

    // Repository operates on RunPoint; telemetry points carry the same geo payload
    std::vector<gameplay::workout_tracking::RunPoint> track_points;
    track_points.reserve(game_points.size());
    for (std::size_t i = 0; i < game_points.size(); ++i)
    {
        auto& p = track_points.emplace_back();
        p.run_id = static_cast<int64_t>(*run_id);
        p.point_order = static_cast<int32_t>(i);
        p.coord = game_points[i].coord;
        p.speed_ms = game_points[i].speed_ms;
        p.recorded_at = game_points[i].recorded_at;
    }

    const bool persisted =
        co_await m_track_repository->PushTrackBatchAsync(*run_id, track_points, total_delta_meters, delta_points);
    if (!persisted)
    {
        // Release the claim so the client retry can re-ingest this batch
        std::ignore = co_await m_redis->DeleteAsync(dedup_key);
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    // Live progress snapshot: written stats already include this batch (same
    // pipeline flush precedes the read)
    response.set_accepted_points_count(static_cast<int32_t>(game_points.size()));
    response.set_server_timestamp_ms(GetCurrentTimestampMs());
    if (auto stats = co_await m_track_repository->GetActiveRunStatsAsync(batch.session_id()); stats.has_value())
    {
        response.set_total_distance_meters(stats->distance_meters);
        response.set_total_points_count(stats->points_count);
    }

    // RADAR (live position): last valid point only, fresh enough to be online.
    // Channel sharding by h3_zone happens inside ShardedRedisManager::PublishAsync.
    const std::string run_key = MakeActiveRunKey(batch.session_id());
    const auto visibility_raw = co_await m_redis->HGetAsync(run_key, "visibility");
    const auto visibility = visibility_raw.has_value() && !visibility_raw->empty()
                                ? static_cast<run::proto::session::VisibilityScope>(std::stoi(*visibility_raw))
                                : run::proto::session::VISIBILITY_PRIVATE;

    if (visibility != run::proto::session::VISIBILITY_PRIVATE)
    {
        const auto& current_pos = game_points.back();
        const int64_t latency_ms = GetCurrentTimestampMs() - current_pos.RecordedAtMs();
        // Publish to radar only if the runner is actually online right now
        if (latency_ms < 15'000)
        {
            const uint64_t current_hex =
                m_h3_service.PointToH3(current_pos.coord.latitude, current_pos.coord.longitude, 9).value_or(0);
            const auto team_raw = co_await m_redis->HGetAsync(run_key, "team_id");
            const int64_t team_id = team_raw.has_value() && !team_raw->empty() ? std::stoll(*team_raw) : 0;

            const std::string channel = (visibility == run::proto::session::VISIBILITY_PUBLIC)
                                            ? fmt::format("kazan:zone:{:x}:radar:public", current_hex)
                                            : fmt::format("kazan:zone:{:x}:radar:team:{}", current_hex, team_id);

            gamemap::RadarPing radar_ping;
            radar_ping.set_user_id(static_cast<uint64_t>(user_id));
            radar_ping.set_team_id(static_cast<uint64_t>(team_id));
            radar_ping.set_latitude(current_pos.coord.latitude);
            radar_ping.set_longitude(current_pos.coord.longitude);
            radar_ping.set_speed(current_pos.speed_ms.value_or(0.0f));
            radar_ping.set_timestamp(current_pos.RecordedAtMs());
            std::ignore = co_await m_redis->PublishAsync(channel, radar_ping.SerializeAsString());
        }
    }

    co_return response;
}

Task<run::proto::session::FinishRunResponse> RunService::FinishRunAsync(int64_t user_id, const run::proto::session::FinishRunRequest& request)
{
    run::proto::session::FinishRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(run::proto::session::FINISH_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());

    // total_distance_meters
    // duration_seconds
    // hexes_claimed_count
    // total_score
    // average_speed_kmh

    co_return response;
}

int64_t RunService::GetCurrentTimestampMs() const noexcept
{
    const auto now = std::chrono::system_clock::now();
    const auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

} // namespace run::service
