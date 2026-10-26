#include "network/server/envelope_dispatcher.hpp"
#include "monitoring/metrics_collector.hpp"
#include <chrono>

namespace {
const char* PayloadCaseToString(run::proto::Envelope::PayloadCase c)
{
    switch (c)
    {
    case run::proto::Envelope::PayloadCase::kLocationBatch: return "LocationBatch";
    case run::proto::Envelope::PayloadCase::kStartRunRequest: return "StartRunRequest";
    case run::proto::Envelope::PayloadCase::kGetActiveRunRequest: return "GetActiveRunRequest";
    case run::proto::Envelope::PayloadCase::kFinishRunRequest: return "FinishRunRequest";
    case run::proto::Envelope::PayloadCase::kCancelRunRequest: return "CancelRunRequest";
    case run::proto::Envelope::PayloadCase::kOfflineRunInitRequest: return "OfflineRunInitRequest";
    case run::proto::Envelope::PayloadCase::kOfflineRunChunkRequest: return "OfflineRunChunkRequest";
    case run::proto::Envelope::PayloadCase::kOfflineRunCommitRequest: return "OfflineRunCommitRequest";
    case run::proto::Envelope::PayloadCase::kSubscribeViewportRequest: return "SubscribeViewportRequest";
    default: return "other";
    }
}
} // namespace

#include "envelope.pb.h"

namespace run::server
{

boost::asio::awaitable<std::optional<run::proto::Envelope>>
DispatchAsync(int64_t user_id, const run::proto::Envelope& request, service::running::ServiceStack& services)
{
    if (request.payload_case() == run::proto::Envelope::PAYLOAD_NOT_SET)
    {
        co_return std::nullopt;
    }

    auto start_time = std::chrono::steady_clock::now();
    monitoring::MetricsCollector::Instance().IncrementWsEnvelopes(PayloadCaseToString(request.payload_case()));

    auto response = MakeResponseEnvelope(request);

    using Case = run::proto::Envelope::PayloadCase;
    switch (request.payload_case())
    {
    // --- Telemetry streaming -------------------------------------------
    case Case::kLocationBatch:
        *response.mutable_batch_process_response() =
            co_await services.telemetry_ingestor.ProcessBatchAsync(user_id, request.location_batch());
        break;

    // --- Session lifecycle ----------------------------------------------
    case Case::kStartRunRequest:
        *response.mutable_start_run_response() =
            co_await services.run_session_service.StartRunAsync(user_id, request.start_run_request());
        break;
    case Case::kGetActiveRunRequest:
        *response.mutable_get_active_run_response() = co_await services.run_session_service.GetActiveRunAsync(user_id);
        break;
    case Case::kFinishRunRequest:
        *response.mutable_finish_run_response() =
            co_await services.run_session_service.FinishRunAsync(user_id, request.finish_run_request());
        break;
    case Case::kCancelRunRequest:
        *response.mutable_cancel_run_response() =
            co_await services.run_session_service.CancelRunAsync(user_id, request.cancel_run_request());
        break;

    // --- Offline upload ---------------------------------------------------
    case Case::kOfflineRunInitRequest:
        *response.mutable_offline_run_init_response() =
            co_await services.offline_upload_service.InitAsync(user_id, request.offline_run_init_request());
        break;
    case Case::kOfflineRunChunkRequest:
        *response.mutable_offline_run_chunk_response() =
            co_await services.offline_upload_service.ChunkAsync(user_id, request.offline_run_chunk_request());
        break;
    case Case::kOfflineRunCommitRequest:
        *response.mutable_offline_run_commit_response() =
            co_await services.offline_upload_service.CommitAsync(user_id, request.offline_run_commit_request());
        break;

    // --- Server -> client events and unknown payloads ----------------------
    case Case::kBatchProcessResponse:
    case Case::kStartRunResponse:
    case Case::kGetActiveRunResponse:
    case Case::kFinishRunResponse:
    case Case::kCancelRunResponse:
    case Case::kOfflineRunInitResponse:
    case Case::kOfflineRunChunkResponse:
    case Case::kOfflineRunCommitResponse:
    case Case::kSubscribeViewportRequest:
    case Case::kSubscribeViewportResponse:
    case Case::kHexagonCaptureEvent:
    case Case::kRadarPing:
        LOG_WARN("[dispatcher] unexpected payload case {} from user {}", static_cast<int>(request.payload_case()),
                 user_id);
        co_return std::nullopt;
    case Case::PAYLOAD_NOT_SET:
        co_return std::nullopt;
    }

    auto end_time = std::chrono::steady_clock::now();
    double duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    monitoring::MetricsCollector::Instance().ObserveWsDispatchDuration(duration_us);

    co_return response;
}

} // namespace run::server
