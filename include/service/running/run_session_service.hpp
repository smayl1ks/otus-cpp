#pragma once

#include "repository/i_active_run_store.hpp"
#include "repository/i_run_history_repository.hpp"
#include "service/running/run_finalizer.hpp"
#include "session.pb.h"

#include <boost/asio/awaitable.hpp>
#include <memory>

namespace run::service::running
{

namespace net = boost::asio;

struct SessionServiceConfig
{
    uint32_t max_points_per_batch{100}; // must match GpsAntiCheatConfig::max_points_per_batch
    uint32_t batch_interval_hint_ms{5'000};
};

// Session lifecycle: Start / GetActiveRun (reconnect sync) / Finish / Cancel.
class RunSessionService
{
public:
    // Shared outcome of the close pipeline; FinishRun/CancelRun map it onto
    // their own proto reject reasons.
    struct CloseRunOutcome
    {
        enum class Status
        {
            kClosed,
            kAlreadyClosed,
            kNotFound,
            kFailed, // finalize refused (concurrent close) or persistence failed
        };

        Status status{Status::kNotFound};
        std::optional<repository::StoredRunSummary> summary;
    };

    RunSessionService(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                      std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                      std::shared_ptr<RunFinalizer> run_finalizer, SessionServiceConfig config = {});

    net::awaitable<run::proto::session::StartRunResponse>
    StartRunAsync(int64_t user_id, const run::proto::session::StartRunRequest& request);

    net::awaitable<run::proto::session::GetActiveRunResponse> GetActiveRunAsync(int64_t user_id);

    net::awaitable<run::proto::session::FinishRunResponse>
    FinishRunAsync(int64_t user_id, const run::proto::session::FinishRunRequest& request);

    net::awaitable<run::proto::session::CancelRunResponse>
    CancelRunAsync(int64_t user_id, const run::proto::session::CancelRunRequest& request);

private:
    // Finish and Cancel differ only in the close reason and the response
    // shape — the pipeline (idempotency guard, ownership check, track fetch,
    // finalize) lives here, once.
    net::awaitable<CloseRunOutcome>
    CloseRunAsync(int64_t user_id, const std::string& session_id, repository::RunCloseReason close_reason);

    std::shared_ptr<repository::IActiveRunStore> m_run_store;
    std::shared_ptr<repository::IRunHistoryRepository> m_run_history;
    std::shared_ptr<RunFinalizer> m_run_finalizer;
    SessionServiceConfig m_config;
};

} // namespace run::service::running
