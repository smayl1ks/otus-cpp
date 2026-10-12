#pragma once

#include "domain/gps_anticheat.hpp"
#include "domain/h3_capture_resolver.hpp"
#include "domain/h3_service.hpp"
#include "domain/run_validator.hpp"

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

struct BatchProcessResult
{
    common::SystemStatus system_status{common::SYSTEM_STATUS_OK};
    common::AnticheatStatus anticheat_status{common::ANTICHEAT_STATUS_OK};
};

struct StartRunResult
{
    common::SystemStatus system_status{common::SYSTEM_STATUS_OK};
    events::StartRunRejectReason reject_reason{events::START_REJECT_REASON_NONE};
    events::StartRunResponse response_payload;
};

struct FinishRunResult
{
    common::SystemStatus system_status{common::SYSTEM_STATUS_OK};
    events::FinishRunRejectReason reject_reason{events::FINISH_REJECT_REASON_NONE};
    events::FinishRunResponse response_payload;
};

class RunService
{
public:
    Task<StartRunResult> StartRunAsync(int64_t user_id, const events::StartRunRequest& request);
    Task<BatchProcessResult> ProcessBatchAsync(int64_t user_id, const run::proto::location::LocationBatch& batch);
    Task<FinishRunResult> FinishRunAsync(int64_t user_id, const events::FinishRunRequest& request);

private:
    int64_t GetCurrentTimestampMs() const noexcept;

private:
    // std::shared_ptr<repository::IRunRepository> m_run_repository;
    // std::shared_ptr<repository::IHexagonRepository> m_hex_repository;
    // std::shared_ptr<cache::IRedisCache> m_cache;

    domain::anticheat::GpsAntiCheat m_anticheat;
    domain::validator::RunValidator m_run_validator;
    domain::h3::H3Service m_h3_service;
};

} // namespace run::service