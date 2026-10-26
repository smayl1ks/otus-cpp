#include "network/server/envelope_dispatcher.hpp"

#include "envelope.pb.h"

namespace run::network::server
{

boost::asio::awaitable<std::optional<run::proto::Envelope>>
DispatchAsync(int64_t user_id, const run::proto::Envelope& request, service::running::ServiceStack& services)
{
    if (request.payload_case() == run::proto::Envelope::PAYLOAD_NOT_SET)
    {
        co_return std::nullopt;
    }

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

    co_return response;
}

} // namespace run::network::server
