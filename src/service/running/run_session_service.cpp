#include "service/running/run_session_service.hpp"

#include "logger/logger.hpp"
#include "network/mappers/session_mapper.hpp"

#include <chrono>
#include <utility>

namespace run::service::running
{

using run::proto::session::StartRunRejectReason;
using run::proto::session::FinishRunRejectReason;
using run::proto::session::CancelRunRejectReason;

RunSessionService::RunSessionService(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                                     std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                                     std::shared_ptr<RunFinalizer> run_finalizer, SessionServiceConfig config)
    : m_store(std::move(active_run_store)), m_history(std::move(history_repository)),
      m_finalizer(std::move(run_finalizer)), m_config(config)
{
}

int64_t RunSessionService::NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

boost::asio::awaitable<run::proto::session::StartRunResponse>
RunSessionService::StartRunAsync(int64_t user_id, const run::proto::session::StartRunRequest& request)
{
    run::proto::session::StartRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(StartRunRejectReason::START_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_max_points_per_batch(m_config.max_points_per_batch);
    response.set_batch_interval_hint_ms(m_config.batch_interval_hint_ms);

    // [1] Payload guard.
    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    // [2] Idempotent start: a known live session_id (app restart, reconnect)
    // resumes instead of starting a second run.
    if (auto existing = co_await m_store->GetActiveRunAsync(request.session_id());
        existing.has_value() && existing->user_id == user_id)
    {
        LOG_INFO("[session] resuming session '{}' for user {}", request.session_id(), user_id);
        response.set_resumed(true);
        *response.mutable_snapshot() = network::mappers::BuildSnapshot(*existing);
        response.set_server_start_timestamp(existing->started_at_ms);
        response.set_server_timestamp_ms(NowMs());
        co_return response;
    }

    // [3] One active run per user (atomic claim; the second concurrent device loses).
    if (!(co_await m_store->TryClaimUserSlotAsync(user_id, request.session_id())))
    {
        LOG_INFO("[session] start rejected for user {}: another run is active", user_id);
        response.set_reject_reason(StartRunRejectReason::START_REJECT_REASON_ACTIVE_SESSION_EXISTS);
        response.set_server_timestamp_ms(NowMs());
        co_return response;
    }

    // [4] PostgreSQL: the runs row exists from the first millisecond, giving
    // the run its server_run_id (user request: write at start).
    const int64_t started_at_ms = NowMs();
    repository::NewRunRecord new_run;
    new_run.session_id = request.session_id();
    new_run.user_id = user_id;
    new_run.team_id = request.team_id();
    new_run.mode = network::mappers::ToDomain(request.mode());
    new_run.visibility = network::mappers::ToDomain(request.visibility());
    new_run.started_at_ms = started_at_ms;
    new_run.event_id = request.event_id();
    new_run.device_id = request.device_id();
    new_run.app_version = request.app_version();

    auto server_run_id = co_await m_history->CreateRunAsync(new_run);
    if (!server_run_id.has_value())
    {
        LOG_ERROR("[session] history CreateRun failed for session '{}'", request.session_id());
        co_await m_store->ReleaseUserSlotAsync(user_id, request.session_id());
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    // [5] Redis live state.
    repository::ActiveRunRecord record;
    record.session_id = request.session_id();
    record.server_run_id = *server_run_id;
    record.user_id = user_id;
    record.team_id = request.team_id();
    record.mode = new_run.mode;
    record.visibility = new_run.visibility;
    record.started_at_ms = started_at_ms;

    if (!(co_await m_store->InitActiveRunAsync(record)))
    {
        co_await m_store->ReleaseUserSlotAsync(user_id, request.session_id());
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    LOG_INFO("[session] started session '{}' (run {}, user {}, mode {})", record.session_id, record.server_run_id,
             user_id, new_run.mode == repository::RunMode::kOffline ? "offline" : "online");

    response.set_server_run_id(record.server_run_id);
    response.set_server_start_timestamp(started_at_ms);
    response.set_server_timestamp_ms(NowMs());
    co_return response;
}

boost::asio::awaitable<run::proto::session::GetActiveRunResponse> RunSessionService::GetActiveRunAsync(int64_t user_id)
{
    run::proto::session::GetActiveRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_max_points_per_batch(m_config.max_points_per_batch);
    response.set_batch_interval_hint_ms(m_config.batch_interval_hint_ms);
    response.set_server_timestamp_ms(NowMs());

    if (auto record = co_await m_store->GetActiveRunByUserAsync(user_id); record.has_value())
    {
        *response.mutable_snapshot() = network::mappers::BuildSnapshot(*record);
    }
    co_return response;
}

boost::asio::awaitable<run::proto::session::FinishRunResponse>
RunSessionService::FinishRunAsync(int64_t user_id, const run::proto::session::FinishRunRequest& request)
{
    run::proto::session::FinishRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(FinishRunRejectReason::FINISH_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_server_timestamp_ms(NowMs());

    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    // [1] Idempotent finish: a retry after a lost ack returns the stored summary.
    if (auto stored = co_await m_store->GetRunSummaryAsync(request.session_id());
        stored.has_value() && stored->user_id == user_id)
    {
        LOG_INFO("[session] finish retry for already closed session '{}'", request.session_id());
        response.set_reject_reason(FinishRunRejectReason::FINISH_REJECT_REASON_ALREADY_FINISHED);
        *response.mutable_summary() = network::mappers::BuildSummary(*stored);
        co_return response;
    }

    // [2] The session must be live and belong to the caller.
    auto record = co_await m_store->GetActiveRunAsync(request.session_id());
    if (!record.has_value() || record->user_id != user_id)
    {
        response.set_reject_reason(FinishRunRejectReason::FINISH_REJECT_REASON_SESSION_NOT_FOUND);
        co_return response;
    }

    // [3] Single closing path: anticheat replay -> captures -> thresholds -> PG.
    auto track = co_await m_store->GetTrackAsync(request.session_id());
    auto summary = co_await m_finalizer->FinalizeAsync(*record, repository::RunCloseReason::kFinished, track,
                                                       NowMs());
    if (!summary.has_value())
    {
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    *response.mutable_summary() = network::mappers::BuildSummary(*summary);
    co_return response;
}

boost::asio::awaitable<run::proto::session::CancelRunResponse>
RunSessionService::CancelRunAsync(int64_t user_id, const run::proto::session::CancelRunRequest& request)
{
    run::proto::session::CancelRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(CancelRunRejectReason::CANCEL_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_server_timestamp_ms(NowMs());

    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    if (auto stored = co_await m_store->GetRunSummaryAsync(request.session_id());
        stored.has_value() && stored->user_id == user_id)
    {
        response.set_reject_reason(CancelRunRejectReason::CANCEL_REJECT_REASON_ALREADY_FINISHED);
        *response.mutable_summary() = network::mappers::BuildSummary(*stored);
        co_return response;
    }

    auto record = co_await m_store->GetActiveRunAsync(request.session_id());
    if (!record.has_value() || record->user_id != user_id)
    {
        response.set_reject_reason(CancelRunRejectReason::CANCEL_REJECT_REASON_SESSION_NOT_FOUND);
        co_return response;
    }

    auto track = co_await m_store->GetTrackAsync(request.session_id());
    auto summary = co_await m_finalizer->FinalizeAsync(*record, repository::RunCloseReason::kCancelled, track,
                                                       NowMs());
    if (!summary.has_value())
    {
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    *response.mutable_summary() = network::mappers::BuildSummary(*summary);
    co_return response;
}

} // namespace run::service::running
