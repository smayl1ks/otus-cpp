#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/territory_conquest/capture_resolver.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"
#include "repository/i_track_repository.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

#include "common.pb.h"
#include "events.pb.h"
#include "location.pb.h"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace run::service
{

template <typename T>
using Task = boost::asio::awaitable<T>;

struct FinishRunResult
{
    common::SystemStatus system_status{common::SYSTEM_STATUS_OK};
    events::FinishRunRejectReason reject_reason{events::FINISH_REJECT_REASON_NONE};
    events::FinishRunResponse response_payload;
};

class RunService
{
public:
    explicit RunService(std::shared_ptr<storage::redis::ShardedRedisManager> redis_manager,
                        std::shared_ptr<repository::ITrackRepository> track_repository,
                        gameplay::territory_conquest::H3CaptureConfig h3_config = {},
                        gameplay::anticheat::GpsAntiCheatConfig anticheat_config = {},
                        gameplay::workout_tracking::RunValidationConfig validation_config = {});

    ~RunService() = default;

    Task<events::StartRunResponse> StartRunAsync(int64_t user_id, const events::StartRunRequest& request);
    Task<events::BatchProcessResponse> ProcessBatchAsync(int64_t user_id,
                                                         const run::proto::location::LocationBatch& batch);
    Task<events::FinishRunResponse> FinishRunAsync(int64_t user_id, const events::FinishRunRequest& request);

    std::shared_ptr<repository::ITrackRepository> GetTrackRepository() const noexcept
    {
        return m_track_repository;
    }

    std::shared_ptr<storage::redis::ShardedRedisManager> GetRedisManager() const noexcept
    {
        return m_redis;
    }

private:
    int64_t GetCurrentTimestampMs() const noexcept;

private:
    std::shared_ptr<storage::redis::ShardedRedisManager> m_redis;
    std::shared_ptr<repository::ITrackRepository> m_track_repository;

    gameplay::anticheat::GpsAntiCheat m_anticheat;
    gameplay::workout_tracking::RunValidator m_run_validator;
    gameplay::territory_conquest::H3Service m_h3_service;
    std::unique_ptr<gameplay::territory_conquest::H3CaptureResolver> m_capture_resolver;
};

} // namespace run::service
