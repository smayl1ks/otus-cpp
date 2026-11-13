#include "service/running/run_session_service.hpp"

#include "logger/logger.hpp"
#include "network/mappers/session_mapper.hpp"
#include "utils/time.hpp"

#include <utility>

namespace run::service::running
{

using run::proto::session::CancelRunRejectReason;
using run::proto::session::FinishRunRejectReason;
using run::proto::session::StartRunRejectReason;

namespace
{

run::proto::session::StartRunResponse BuildResumeResponse(const repository::ActiveRunRecord& existing,
                                                          const SessionServiceConfig& config)
{
    run::proto::session::StartRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(StartRunRejectReason::START_REJECT_REASON_NONE);
    response.set_session_id(existing.session_id);
    response.set_max_points_per_batch(config.max_points_per_batch);
    response.set_batch_interval_hint_ms(config.batch_interval_hint_ms);
    response.set_resumed(true);
    *response.mutable_snapshot() = network::mappers::BuildSnapshot(existing);
    response.set_server_start_timestamp(existing.started_at_ms);
    response.set_server_timestamp_ms(utils::NowMs());
    return response;
}

} // namespace

RunSessionService::RunSessionService(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                                     std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                                     std::shared_ptr<RunFinalizer> run_finalizer, SessionServiceConfig config)
    : m_run_store(std::move(active_run_store)), m_run_history(std::move(history_repository)),
      m_run_finalizer(std::move(run_finalizer)), m_config(config)
{
}

net::awaitable<run::proto::session::StartRunResponse>
RunSessionService::StartRunAsync(int64_t user_id, const run::proto::session::StartRunRequest& request)
{
    run::proto::session::StartRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(StartRunRejectReason::START_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_max_points_per_batch(m_config.max_points_per_batch);
    response.set_batch_interval_hint_ms(m_config.batch_interval_hint_ms);

    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    if (auto existing = co_await m_run_store->GetActiveRunAsync(request.session_id());
        existing.has_value() && existing->user_id == user_id)
    {
        LOG_INFO("[session] resuming session '{}' for user {}", request.session_id(), user_id);
        co_return BuildResumeResponse(*existing, m_config);
    }

    if (!(co_await m_run_store->TryClaimUserSlotAsync(user_id, request.session_id())))
    {
        if (auto active = co_await m_run_store->GetActiveRunByUserAsync(user_id);
            active.has_value() && active->session_id == request.session_id())
        {
            LOG_INFO("[session] resuming session '{}' for user {} (lost claim retry)", request.session_id(), user_id);
            co_return BuildResumeResponse(*active, m_config);
        }

        LOG_INFO("[session] start rejected for user {}: another run is active", user_id);
        response.set_reject_reason(StartRunRejectReason::START_REJECT_REASON_ACTIVE_SESSION_EXISTS);
        response.set_server_timestamp_ms(utils::NowMs());
        co_return response;
    }

    const int64_t started_at_ms = utils::NowMs();
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

    auto server_run_id = co_await m_run_history->CreateRunAsync(new_run);
    if (!server_run_id.has_value())
    {
        LOG_ERROR("[session] history CreateRun failed for session '{}'", request.session_id());
        co_await m_run_store->ReleaseUserSlotAsync(user_id, request.session_id());
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    repository::ActiveRunRecord record;
    record.session_id = request.session_id();
    record.server_run_id = *server_run_id;
    record.user_id = user_id;
    record.team_id = request.team_id();
    record.mode = new_run.mode;
    record.visibility = new_run.visibility;
    record.started_at_ms = started_at_ms;

    if (!(co_await m_run_store->InitActiveRunAsync(record)))
    {
        co_await m_run_store->ReleaseUserSlotAsync(user_id, request.session_id());
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    LOG_INFO("[session] started session '{}' (run {}, user {}, mode {})", record.session_id, record.server_run_id,
             user_id, new_run.mode == repository::RunMode::kOffline ? "offline" : "online");

    response.set_server_run_id(record.server_run_id);
    response.set_server_start_timestamp(started_at_ms);
    response.set_server_timestamp_ms(utils::NowMs());
    co_return response;
}

net::awaitable<run::proto::session::GetActiveRunResponse> RunSessionService::GetActiveRunAsync(int64_t user_id)
{
    run::proto::session::GetActiveRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_max_points_per_batch(m_config.max_points_per_batch);
    response.set_batch_interval_hint_ms(m_config.batch_interval_hint_ms);
    response.set_server_timestamp_ms(utils::NowMs());

    if (auto record = co_await m_run_store->GetActiveRunByUserAsync(user_id); record.has_value())
    {
        *response.mutable_snapshot() = network::mappers::BuildSnapshot(*record);
    }
    co_return response;
}

net::awaitable<RunSessionService::CloseRunOutcome>
RunSessionService::CloseRunAsync(int64_t user_id, const std::string& session_id,
                                 repository::RunCloseReason close_reason)
{
    if (auto stored = co_await m_run_store->GetRunSummaryAsync(session_id);
        stored.has_value() && stored->user_id == user_id)
    {
        LOG_INFO("[session] close retry for already closed session '{}'", session_id);
        co_return CloseRunOutcome{CloseRunOutcome::Status::kAlreadyClosed, std::move(stored)};
    }

    auto record = co_await m_run_store->GetActiveRunAsync(session_id);
    if (!record.has_value() || record->user_id != user_id)
    {
        if (auto stored = co_await m_run_history->GetFinalizedRunAsync(session_id, user_id); stored.has_value())
        {
            LOG_INFO("[session] recovered finalized run '{}' from PostgreSQL", session_id);
            co_await m_run_store->SaveRunSummaryAsync(*stored);
            co_await m_run_store->DropRunDataAsync(session_id, user_id);
            co_return CloseRunOutcome{CloseRunOutcome::Status::kAlreadyClosed, std::move(stored)};
        }

        // Offline uploads hold the user slot without an active-run record
        if (close_reason == repository::RunCloseReason::kCancelled)
        {
            if (auto meta = co_await m_run_store->GetUploadMetaAsync(session_id);
                meta.has_value() && meta->user_id == user_id)
            {
                repository::ActiveRunRecord run;
                run.session_id = meta->session_id;
                run.server_run_id = meta->server_run_id;
                run.user_id = meta->user_id;
                run.team_id = meta->team_id;
                run.mode = repository::RunMode::kOffline;
                run.visibility = meta->visibility;
                run.started_at_ms = meta->start_timestamp_ms;

                LOG_INFO("[session] aborting offline upload of session '{}' (user {})", session_id, user_id);
                auto summary = co_await m_run_finalizer->FinalizeAsync(run, repository::RunCloseReason::kCancelled, {},
                                                                       utils::NowMs());
                if (!summary.has_value())
                {
                    co_return CloseRunOutcome{CloseRunOutcome::Status::kFailed, std::nullopt};
                }
                co_return CloseRunOutcome{CloseRunOutcome::Status::kClosed, std::move(summary)};
            }
        }
        co_return CloseRunOutcome{CloseRunOutcome::Status::kNotFound, std::nullopt};
    }

    auto track = co_await m_run_store->GetTrackAsync(session_id);
    auto summary = co_await m_run_finalizer->FinalizeAsync(*record, close_reason, track, utils::NowMs());
    if (!summary.has_value())
    {
        co_return CloseRunOutcome{CloseRunOutcome::Status::kFailed, std::nullopt};
    }
    co_return CloseRunOutcome{CloseRunOutcome::Status::kClosed, std::move(summary)};
}

net::awaitable<run::proto::session::FinishRunResponse>
RunSessionService::FinishRunAsync(int64_t user_id, const run::proto::session::FinishRunRequest& request)
{
    run::proto::session::FinishRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(FinishRunRejectReason::FINISH_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_server_timestamp_ms(utils::NowMs());

    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    const auto outcome = co_await CloseRunAsync(user_id, request.session_id(), repository::RunCloseReason::kFinished);
    switch (outcome.status)
    {
    case CloseRunOutcome::Status::kAlreadyClosed:
        response.set_reject_reason(FinishRunRejectReason::FINISH_REJECT_REASON_ALREADY_FINISHED);
        *response.mutable_summary() = network::mappers::BuildSummary(*outcome.summary);
        break;
    case CloseRunOutcome::Status::kNotFound:
        response.set_reject_reason(FinishRunRejectReason::FINISH_REJECT_REASON_SESSION_NOT_FOUND);
        break;
    case CloseRunOutcome::Status::kFailed:
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        break;
    case CloseRunOutcome::Status::kClosed:
        *response.mutable_summary() = network::mappers::BuildSummary(*outcome.summary);
        break;
    }
    co_return response;
}

net::awaitable<run::proto::session::CancelRunResponse>
RunSessionService::CancelRunAsync(int64_t user_id, const run::proto::session::CancelRunRequest& request)
{
    run::proto::session::CancelRunResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(CancelRunRejectReason::CANCEL_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_server_timestamp_ms(utils::NowMs());

    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }

    const auto outcome = co_await CloseRunAsync(user_id, request.session_id(), repository::RunCloseReason::kCancelled);
    switch (outcome.status)
    {
    case CloseRunOutcome::Status::kAlreadyClosed:
        response.set_reject_reason(CancelRunRejectReason::CANCEL_REJECT_REASON_ALREADY_FINISHED);
        *response.mutable_summary() = network::mappers::BuildSummary(*outcome.summary);
        break;
    case CloseRunOutcome::Status::kNotFound:
        response.set_reject_reason(CancelRunRejectReason::CANCEL_REJECT_REASON_SESSION_NOT_FOUND);
        break;
    case CloseRunOutcome::Status::kFailed:
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        break;
    case CloseRunOutcome::Status::kClosed:
        *response.mutable_summary() = network::mappers::BuildSummary(*outcome.summary);
        break;
    }
    co_return response;
}

} // namespace run::service::running
