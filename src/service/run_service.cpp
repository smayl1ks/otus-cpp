#include "service/run_service.hpp"

namespace run::service
{

Task<StartRunResult> RunService::StartRunAsync(int64_t user_id, const events::StartRunRequest& request)
{
    StartRunResult result;

    if (request.session_id().empty())
    {
        result.system_status = common::SYSTEM_STATUS_INVALID_PAYLOAD;
        co_return result;
    }

    // 2. Check if the user is suspended or blocked from running/territory capture
    // if (co_await m_user_repo->IsUserBlockedAsync(user_id, boost::asio::use_awaitable))
    // {
    //     result.reject_reason = events::START_REJECT_REASON_USER_BLOCKED;
    //     co_return result;
    // }

    // 3. Check if the user already has an ongoing active session
    // if (co_await m_cache->HasActiveSessionAsync(user_id, boost::asio::use_awaitable))
    // {
    //     result.reject_reason = events::START_REJECT_REASON_ACTIVE_SESSION_EXISTS;
    //     co_return result;
    // }

    // 4. Validate faction_id (ensure faction exists and is active)
    // if (!co_await m_faction_repo->FactionExistsAsync(request.faction_id(), boost::asio::use_awaitable))
    // {
    //     result.reject_reason = events::START_REJECT_REASON_INVALID_FACTION;
    //     co_return result;
    // }

    // 5. Generate server-validated start timestamp (protects against device time spoofing)
    const int64_t server_start_timestamp = GetCurrentTimestampMs();

    // 6. Register active session in Redis cache mapping user_id <-> session_id
    // co_await m_cache->CreateActiveSessionAsync(user_id, request.session_id(), request.faction_id(),
    //                                            server_start_timestamp, boost::asio::use_awaitable);

    // 7. Populate success response payload
    // result.response_payload.set_session_id(request.session_id());
    // result.response_payload.set_server_start_timestamp(server_start_timestamp);

    co_return result;
}

Task<BatchProcessResult> RunService::ProcessBatchAsync(int64_t user_id,
                                                       const run::proto::location::LocationBatch& batch)
{
    if (batch.batch_uuid().empty())
    {
        co_return BatchProcessResult{.system_status = common::SYSTEM_STATUS_INVALID_PAYLOAD};
    }

    // 1. Idempotency check via Redis
    // if (co_await m_cache->IsBatchProcessedAsync(batch.batch_uuid(), boost::asio::use_awaitable))
    // {
    //     co_return BatchProcessResult{}; // Already processed, return OK
    // }

    // 2. Anti-cheat validation using defined proto status codes
    auto anticheat_result = m_anticheat.ValidateBatch(batch, GetCurrentTimestampMs());
    if (anticheat_result != common::ANTICHEAT_STATUS_OK)
    {
        co_return BatchProcessResult{
            .system_status = common::SYSTEM_STATUS_OK, // Request reached server successfully
            .anticheat_status = anticheat_result       // Specific cheat violation reason
        };
    }

    // 3. Buffer raw points in Redis for active session tracking
    // co_await m_run_repo->AppendBatchPointsToBufferAsync(batch.session_id(), batch.points(),
    // boost::asio::use_awaitable);

    // 4. Handle run completion state
    // if (batch.state() == run::proto::location::RUNNING_STATE_FINISHED)
    // {
    //     co_await FinalizeRunSessionAsync(user_id, batch.session_id());
    // }

    // 5. Mark batch as processed in Redis cache
    // co_await m_cache->MarkBatchProcessedAsync(batch.batch_uuid(), boost::asio::use_awaitable);
    co_return BatchProcessResult{};
}

Task<FinishRunResult> RunService::FinishRunAsync(int64_t user_id, const events::FinishRunRequest& request)
{
    FinishRunResult result;
    result.response_payload.set_session_id(request.session_id());

    //     // 1. Check if the session exists and is active in Redis/DB
    //     // auto session_opt = co_await m_cache->GetActiveSessionAsync(request.session_id(),
    //     boost::asio::use_awaitable);
    //     // if (!session_opt.has_value())
    //     // {
    //     //     result.reject_reason = events::FINISH_REJECT_REASON_SESSION_NOT_FOUND;
    //     //     co_return result;
    //     // }

    //     // 2. Fetch all accumulated raw points for this session from Redis buffer or DB
    //     // auto points = co_await m_run_repo->GetBufferedSessionPointsAsync(request.session_id(),
    //     // boost::asio::use_awaitable); if (points.empty())
    //     // {
    //     //     result.reject_reason = events::FINISH_REJECT_REASON_INSUFFICIENT_DATA;
    //     //     co_return result;
    //     // }

    //     // 3. Validate run metrics using RunValidator (Min/Max distance & duration)

    //     // auto metrics = m_run_validator.Analyze(points);
    //     // if (!metrics.is_valid)
    //     // {
    //     //     result.reject_reason = events::FINISH_REJECT_REASON_INSUFFICIENT_DATA;
    //     //     co_return result;
    //     // }

    //     // 4. Heavy domain processing: Interpolation, loop detection, and H3 territory capture

    //     domain::h3::TrackInterpolator interpolator{h3_service};
    //     auto run_data = interpolator.ProcessRunTrack(points);

    //     domain::h3::LoopDetector loop_detector{h3_service};
    //     auto loop_result = loop_detector.ProcessLoop(run_data.unique_visited_hexes);

    //     // 5. Persist run session and hex captures in PostgreSQL via transaction
    //     auto persistence_result = co_await m_run_repo->FinalizeAndPersistRunAsync(user_id, request.session_id(),
    //     metrics,
    //                                                                               loop_result,
    //                                                                               boost::asio::use_awaitable);

    //     // 6. Populate success response payload
    //     result.response_payload.set_total_distance_meters(metrics.total_distance_meters);
    //     result.response_payload.set_duration_seconds(static_cast<uint32_t>(metrics.duration_seconds));
    //     result.response_payload.set_hexes_claimed_count(persistence_result.hexes_claimed_count);
    //     result.response_payload.set_total_score(persistence_result.total_score);

    //     // Calculate average speed in km/h: (meters / 1000) / (seconds / 3600)
    //     if (metrics.duration_seconds > 0)
    //     {
    //         double speed_kmh =
    //             (metrics.total_distance_meters / 1000.0) / (static_cast<double>(metrics.duration_seconds) / 3600.0);
    //         result.response_payload.set_average_speed_kmh(static_cast<float>(speed_kmh));
    //     }

    //     // 7. Cleanup active session cache in Redis
    //     co_await m_cache->RemoveActiveSessionAsync(request.session_id(), boost::asio::use_awaitable);

    co_return result;
}

int64_t RunService::GetCurrentTimestampMs() const noexcept
{
    const auto now = std::chrono::system_clock::now();

    const auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

} // namespace run::service