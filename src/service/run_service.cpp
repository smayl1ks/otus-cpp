#include "service/run_service.hpp"
#include "common.pb.h"
#include "location.pb.h"
#include "network/mappers/location_mapper.hpp"

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

Task<events::StartRunResponse> RunService::StartRunAsync(int64_t user_id, const events::StartRunRequest& request)
{
    events::StartRunResponse response;
    response.set_session_id(request.session_id());
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(events::START_REJECT_REASON_NONE);

    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    const bool active_exists = co_await m_track_repository->CheckActiveSessionExistsAsync(user_id);
    if (active_exists)
    {
        response.set_reject_reason(events::START_REJECT_REASON_ACTIVE_SESSION_EXISTS);
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

Task<events::BatchProcessResponse> RunService::ProcessBatchAsync(int64_t user_id,
                                                                 const run::proto::location::LocationBatch& batch)
{
    events::BatchProcessResponse response;

    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_anticheat_status(common::ANTICHEAT_STATUS_OK);
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

    if (batch.state() == run::proto::location::RUNNING_STATE_PAUSED)
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

    // Сохраняем в редис
    

    // Чисто фишка редиса для Live режима
    // -------------------------------------------------------------
    // 3. РАДАР (LIVE POSITION): Только для последней точки и только если онлайн
    // -------------------------------------------------------------
    // if (visibility != events::VISIBILITY_PRIVATE)
    // {
    //     const auto& current_pos = game_points.back();
    //     const int64_t latency_ms = GetCurrentTimestampMs() - current_pos.RecordedAtMs();
    //     // Публикуем на радар только если задержка меньше 15 секунд (бегун реально онлайн)
    //     if (latency_ms < 15'000)
    //     {
    //         const uint64_t current_hex = m_h3_service.CoordToCell(current_pos.coord, 9);
            
    //         const std::string channel = (visibility == events::VISIBILITY_PUBLIC)
    //             ? fmt::format("kazan:zone:{:x}:radar:public", current_hex)
    //             : fmt::format("kazan:zone:{:x}:radar:team:{}", current_hex, team_id);
    //         events::RadarPing radar_ping;
    //         radar_ping.set_user_id(user_id);
    //         radar_ping.set_team_id(team_id);
    //         radar_ping.set_latitude(current_pos.coord.latitude);
    //         radar_ping.set_longitude(current_pos.coord.longitude);
    //         radar_ping.set_speed(current_pos.speed_ms.value_or(0.0f));
    //         co_await m_redis->PublishAsync(channel, radar_ping.SerializeAsString());
    //     }
    // }

    co_return response;
}

Task<events::FinishRunResponse> RunService::FinishRunAsync(int64_t user_id, const events::FinishRunRequest& request)
{
    events::FinishRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(events::FINISH_REJECT_REASON_NONE);
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
