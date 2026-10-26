#pragma once

#include "repository/i_active_run_store.hpp"
#include "repository/i_run_history_repository.hpp"
#include "service/running/run_finalizer.hpp"
#include "session.pb.h"

#include <boost/asio/awaitable.hpp>
#include <memory>

namespace run::service::running
{

// Session lifecycle: Start / GetActiveRun (reconnect sync) / Finish / Cancel.
// Owns the run state machine and the lifecycle idempotency rules:
//   * Start with an already-active session_id  -> resumed=true + snapshot;
//   * Finish/Cancel of an already-closed run   -> stored RunSummary;
//   * "one active run per user"                -> atomic slot claim.
// Closing work is delegated to RunFinalizer.
struct SessionServiceConfig
{
    uint32_t max_points_per_batch{100};    // must match GpsAntiCheatConfig::max_points_per_batch
    uint32_t batch_interval_hint_ms{5'000};
};

class RunSessionService
{
public:
    RunSessionService(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                      std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                      std::shared_ptr<RunFinalizer> run_finalizer, SessionServiceConfig config = {});

    boost::asio::awaitable<run::proto::session::StartRunResponse>
    StartRunAsync(int64_t user_id, const run::proto::session::StartRunRequest& request);

    boost::asio::awaitable<run::proto::session::GetActiveRunResponse> GetActiveRunAsync(int64_t user_id);

    boost::asio::awaitable<run::proto::session::FinishRunResponse>
    FinishRunAsync(int64_t user_id, const run::proto::session::FinishRunRequest& request);

    boost::asio::awaitable<run::proto::session::CancelRunResponse>
    CancelRunAsync(int64_t user_id, const run::proto::session::CancelRunRequest& request);

private:
    static int64_t NowMs() noexcept;

    std::shared_ptr<repository::IActiveRunStore> m_store;
    std::shared_ptr<repository::IRunHistoryRepository> m_history;
    std::shared_ptr<RunFinalizer> m_finalizer;
    SessionServiceConfig m_config;
};

} // namespace run::service::running
