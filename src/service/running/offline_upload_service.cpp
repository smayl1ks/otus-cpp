#include "service/running/offline_upload_service.hpp"

#include "logger/logger.hpp"
#include "network/mappers/location_mapper.hpp"
#include "network/mappers/session_mapper.hpp"

#include <chrono>
#include <utility>

namespace run::service::running
{

using run::proto::session::OfflineRunRejectReason;

namespace
{

using gameplay::anticheat::TelemetryPoint;

std::vector<TelemetryPoint> ToTelemetryPoints(const run::proto::session::OfflineRunChunkRequest& chunk)
{
    std::vector<TelemetryPoint> points;
    points.reserve(static_cast<std::size_t>(chunk.points_size()));
    for (const auto& proto_point : chunk.points())
    {
        points.push_back(network::mappers::LocationMapper::ToTelemetryPoint(proto_point));
    }
    return points;
}

} // namespace

OfflineUploadService::OfflineUploadService(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                                           std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                                           std::shared_ptr<RunFinalizer> run_finalizer, UploadLimits limits)
    : m_store(std::move(active_run_store)), m_history(std::move(history_repository)),
      m_finalizer(std::move(run_finalizer)), m_anticheat(), m_limits(limits)
{
}

int64_t OfflineUploadService::NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

boost::asio::awaitable<run::proto::session::OfflineRunInitResponse>
OfflineUploadService::InitAsync(int64_t user_id, const run::proto::session::OfflineRunInitRequest& request)
{
    run::proto::session::OfflineRunInitResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_max_points_per_chunk(m_limits.max_points_per_chunk);
    response.set_max_total_points(m_limits.max_total_points);
    response.set_server_timestamp_ms(NowMs());

    // [1] Payload and declared time range (the 12 h cap is re-checked over
    // the assembled track at commit; here it just fails fast).
    if (request.session_id().empty())
    {
        response.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return response;
    }
    if (request.finish_timestamp() <= request.start_timestamp() ||
        request.finish_timestamp() - request.start_timestamp() > m_limits.max_duration_seconds * 1000)
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_INVALID_TIME_RANGE);
        co_return response;
    }
    if (request.total_points_count() > m_limits.max_total_points)
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_TOO_MANY_POINTS);
        co_return response;
    }

    // [2] Idempotent init: resume an interrupted upload of the same session.
    if (auto existing = co_await m_store->GetUploadMetaAsync(request.session_id()); existing.has_value())
    {
        if (existing->user_id != user_id)
        {
            response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_SESSION_CONFLICT);
            co_return response;
        }
        const auto accepted = co_await m_store->GetAcceptedChunksAsync(request.session_id());
        LOG_INFO("[upload] resuming upload of session '{}' ({} chunks accepted)", request.session_id(),
                 accepted.size());
        for (const int32_t chunk_index : accepted)
        {
            response.add_accepted_chunk_indexes(chunk_index);
        }
        response.set_server_run_id(existing->server_run_id);
        co_return response;
    }

    // [3] Committed already? Keep replying with the stored summary path via commit.
    if (auto stored = co_await m_store->GetRunSummaryAsync(request.session_id()); stored.has_value())
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_ALREADY_COMMITTED);
        co_return response;
    }

    // [4] The slot is shared with live runs: no upload while another run is active.
    if (!(co_await m_store->TryClaimUserSlotAsync(user_id, request.session_id())))
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_ACTIVE_SESSION_EXISTS);
        co_return response;
    }

    // [5] PostgreSQL row at init (same as a live start).
    repository::NewRunRecord new_run;
    new_run.session_id = request.session_id();
    new_run.user_id = user_id;
    new_run.team_id = request.team_id();
    new_run.mode = repository::RunMode::kOffline;
    new_run.visibility = network::mappers::ToDomain(request.visibility());
    new_run.started_at_ms = request.start_timestamp();
    new_run.event_id = request.event_id();
    new_run.device_id = request.device_id();
    new_run.app_version = request.app_version();

    auto server_run_id = co_await m_history->CreateRunAsync(new_run);
    if (!server_run_id.has_value())
    {
        co_await m_store->ReleaseUserSlotAsync(user_id, request.session_id());
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    repository::UploadMetaRecord meta;
    meta.session_id = request.session_id();
    meta.server_run_id = *server_run_id;
    meta.user_id = user_id;
    meta.team_id = request.team_id();
    meta.visibility = new_run.visibility;
    meta.start_timestamp_ms = request.start_timestamp();
    meta.finish_timestamp_ms = request.finish_timestamp();
    meta.event_id = request.event_id();
    meta.device_id = request.device_id();
    meta.app_version = request.app_version();
    meta.total_chunks = request.total_chunks();

    if (!(co_await m_store->InitUploadAsync(meta)))
    {
        co_await m_store->ReleaseUserSlotAsync(user_id, request.session_id());
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    LOG_INFO("[upload] init session '{}' (run {}, {} chunks declared)", request.session_id(), *server_run_id,
             request.total_chunks());
    response.set_server_run_id(*server_run_id);
    co_return response;
}

boost::asio::awaitable<run::proto::session::OfflineRunChunkResponse>
OfflineUploadService::ChunkAsync(int64_t user_id, const run::proto::session::OfflineRunChunkRequest& request)
{
    run::proto::session::OfflineRunChunkResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_chunk_index(request.chunk_index());

    // [1] The upload must exist and belong to the caller.
    auto meta = co_await m_store->GetUploadMetaAsync(request.session_id());
    if (!meta.has_value() || meta->user_id != user_id)
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_SESSION_NOT_FOUND);
        co_return response;
    }

    // [2] Chunk bookkeeping must match the declaration.
    if (request.chunk_index() < 0 || request.chunk_index() >= meta->total_chunks ||
        request.total_chunks() != meta->total_chunks)
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_SESSION_CONFLICT);
        co_return response;
    }

    const std::vector<TelemetryPoint> points = ToTelemetryPoints(request);

    // [3] Oversized chunk: advisory rejection, client must re-split.
    if (static_cast<uint32_t>(points.size()) > m_limits.max_points_per_chunk)
    {
        response.set_anticheat_status(run::proto::telemetry::ANTICHEAT_STATUS_BATCH_TOO_LARGE);
        co_return response;
    }

    // [4] Store the chunk (dedup by chunk_index inside the store).
    response.set_received_chunks_count(co_await m_store->PutUploadChunkAsync(
        request.session_id(), request.chunk_index(),
        network::mappers::LocationMapper::ToRunPoints(meta->server_run_id, points)));

    // [5] Advisory anticheat preview (old timestamps are expected here; the
    // final verdict happens at commit over the assembled track).
    const auto preview = m_anticheat.ValidateBatch(points, NowMs());
    response.set_anticheat_status(network::mappers::LocationMapper::ToProto(preview));

    LOG_DEBUG("[upload] chunk {} of session '{}' stored, {} chunks held", request.chunk_index(), request.session_id(),
              response.received_chunks_count());
    co_return response;
}

boost::asio::awaitable<run::proto::session::OfflineRunCommitResponse>
OfflineUploadService::CommitAsync(int64_t user_id, const run::proto::session::OfflineRunCommitRequest& request)
{
    run::proto::session::OfflineRunCommitResponse response;
    response.set_system_status(common::SYSTEM_STATUS_OK);
    response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_NONE);
    response.set_session_id(request.session_id());
    response.set_server_timestamp_ms(NowMs());

    // [1] Idempotent commit: the stored summary survives DropRunData, so a
    // retry after a lost ack is answered even though the assembly is gone.
    if (auto stored = co_await m_store->GetRunSummaryAsync(request.session_id());
        stored.has_value() && stored->user_id == user_id)
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_ALREADY_COMMITTED);
        *response.mutable_summary() = network::mappers::BuildSummary(*stored);
        co_return response;
    }

    // [2] The upload must exist and belong to the caller.
    auto meta = co_await m_store->GetUploadMetaAsync(request.session_id());
    if (!meta.has_value() || meta->user_id != user_id)
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_SESSION_NOT_FOUND);
        co_return response;
    }

    // [3] Every declared chunk must be present.
    const auto accepted = co_await m_store->GetAcceptedChunksAsync(request.session_id());
    if (static_cast<int32_t>(accepted.size()) < meta->total_chunks)
    {
        response.set_reject_reason(OfflineRunRejectReason::OFFLINE_REJECT_REASON_UPLOAD_NOT_COMPLETE);
        co_return response;
    }

    // [4] Same closing pipeline as an online finish.
    auto track = co_await m_store->AssembleUploadTrackAsync(request.session_id());

    repository::ActiveRunRecord run;
    run.session_id = meta->session_id;
    run.server_run_id = meta->server_run_id;
    run.user_id = meta->user_id;
    run.team_id = meta->team_id;
    run.mode = repository::RunMode::kOffline;
    run.visibility = meta->visibility;
    run.started_at_ms = meta->start_timestamp_ms;

    auto summary = co_await m_finalizer->FinalizeAsync(run, repository::RunCloseReason::kFinished, track, NowMs());
    if (!summary.has_value())
    {
        response.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        co_return response;
    }

    *response.mutable_summary() = network::mappers::BuildSummary(*summary);
    co_return response;
}

} // namespace run::service::running
