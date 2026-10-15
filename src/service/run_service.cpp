#include "service/run_service.hpp"
#include "network/mappers/location_mapper.hpp"

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

    const int64_t server_start_timestamp = GetCurrentTimestampMs();

    co_return result;
}

Task<BatchProcessResult> RunService::ProcessBatchAsync(int64_t user_id,
                                                       const run::proto::location::LocationBatch& batch)
{
    if (batch.batch_uuid().empty())
    {
        co_return BatchProcessResult{.system_status = common::SYSTEM_STATUS_INVALID_PAYLOAD};
    }

    const auto game_points = network::mappers::LocationMapper::ToTelemetryPoints(batch);

    const auto anticheat_result = m_anticheat.ValidateBatch(game_points, GetCurrentTimestampMs());
    if (anticheat_result != gameplay::anticheat::AnticheatStatus::Ok)
    {
        co_return BatchProcessResult{.system_status = common::SYSTEM_STATUS_OK,
                                     .anticheat_status = network::mappers::LocationMapper::ToProto(anticheat_result)};
    }

    co_return BatchProcessResult{};
}

Task<FinishRunResult> RunService::FinishRunAsync(int64_t user_id, const events::FinishRunRequest& request)
{
    FinishRunResult result;
    result.response_payload.set_session_id(request.session_id());
    co_return result;
}

int64_t RunService::GetCurrentTimestampMs() const noexcept
{
    const auto now = std::chrono::system_clock::now();
    const auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

} // namespace run::service
