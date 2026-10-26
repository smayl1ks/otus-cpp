#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/territory_conquest/capture_resolver.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"
#include "repository/i_active_run_store.hpp"
#include "repository/i_run_history_repository.hpp"

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <span>

namespace run::service::running
{

// The ONLY closing path of a run — used by FinishRun, CancelRun and
// OfflineRunCommit alike, so online and offline runs can never diverge in
// scoring. Pipeline:
//
//   anti-cheat replay over the whole track -> H3 capture resolution (loop
//   closure included) -> validation thresholds (docs/RUN_ROLES.md §3) ->
//   PostgreSQL final transaction -> Redis cleanup (summary for idempotent
//   retries + per-run keys + user slot).
//
// Invalid runs are still stored (is_valid=false + reason): history survives,
// nothing is scored. CPU-bound steps belong on a thread pool at production
// scale; the call stays synchronous for now.
class RunFinalizer
{
public:
    RunFinalizer(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                 std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                 gameplay::anticheat::GpsAntiCheatConfig anticheat_config = {},
                 gameplay::territory_conquest::H3CaptureConfig h3_config = {},
                 gameplay::workout_tracking::RunValidationConfig validation_config = {});

    /// track: live track or assembled offline upload. nullopt = persistence failure,
    /// the run stays open for a client retry.
    boost::asio::awaitable<std::optional<repository::StoredRunSummary>>
    FinalizeAsync(const repository::ActiveRunRecord& run, repository::RunCloseReason close_reason,
                  std::span<const gameplay::workout_tracking::RunPoint> track, int64_t now_ms);

private:
    static repository::ValidationVerdict ClassifyVerdict(const gameplay::workout_tracking::RunMetrics& metrics,
                                                         const gameplay::workout_tracking::RunValidationConfig& config);

    std::shared_ptr<repository::IActiveRunStore> m_store;
    std::shared_ptr<repository::IRunHistoryRepository> m_history;

    gameplay::anticheat::GpsAntiCheat m_anticheat;
    gameplay::territory_conquest::H3Service m_h3_service; // outlives m_capture_resolver
    std::unique_ptr<gameplay::territory_conquest::H3CaptureResolver> m_capture_resolver;
    gameplay::workout_tracking::RunValidator m_run_validator;
};

} // namespace run::service::running
