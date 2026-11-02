#pragma once

#include "context/work_context.hpp"
#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/territory_conquest/capture_resolver.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"
#include "repository/i_active_run_store.hpp"
#include "repository/i_hex_map_cache.hpp"
#include "repository/i_run_history_repository.hpp"
#include "repository/iuser_repository.hpp"

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <optional>
#include <span>

namespace run::service::running
{

namespace net = boost::asio;

// Domain event: one hex changed hands at run close. Transport-free by
// design — the Envelope is assembled in the network layer
// (network/mappers/capture_event_mapper.hpp) so the service layer stays
// proto-free.
struct CaptureNotice
{
    uint64_t h3_index{0};
    int64_t new_owner_id{0};
    std::string new_owner_name;
    std::string new_owner_color_hex;
    std::optional<int64_t> prev_owner_id;
    int32_t score_at_capture{0};
    int64_t timestamp_ms{0};
};

// Outgoing transport for capture fan-out. Implemented in the network layer on
// top of the raw pub/sub publisher (channel formatting + protobuf encoding
// live there); production wiring happens in RunningServices.
struct ICaptureEventPublisher
{
    virtual ~ICaptureEventPublisher() = default;

    /// Best-effort publish of one capture event to its zone channel.
    /// Implementations log failures; they never fail the run.
    virtual net::awaitable<void> PublishAsync(const CaptureNotice& notice) = 0;
};

// The ONLY closing path of a run — used by FinishRun, CancelRun and
// OfflineRunCommit alike, so online and offline runs can never diverge in
// scoring. Phases:
//
//   claim   — atomic closing marker in the active-run store: concurrent
//             closers of the same session are refused, not double-run
//   compute — anticheat replay over the whole track, H3 capture resolution
//             (loop closure included), validation thresholds, scoring. CPU
//             heavy: runs on the WorkContext pool, never on an io thread
//   persist — PostgreSQL final transaction (guarded by run status)
//   fan-out — map-cache updates (batched per zone) + capture event transport
//   cleanup — summary kept for idempotent retries, every per-run key dropped,
//             user slot released, closing marker removed
//
// Invalid runs are still stored (is_valid=false + reason): history survives,
// nothing is scored.
class RunFinalizer
{
public:
    RunFinalizer(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                 std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                 context::WorkContext* work_context,
                 gameplay::anticheat::GpsAntiCheatConfig anticheat_config,
                 gameplay::territory_conquest::H3CaptureConfig h3_config,
                 gameplay::workout_tracking::RunValidationConfig validation_config,
                 std::shared_ptr<repository::IHexMapCache> hex_map_cache,
                 std::shared_ptr<ICaptureEventPublisher> capture_publisher,
                 std::shared_ptr<repository::IUserRepository> user_repository);

    /// track: live track or assembled offline upload. nullopt = the run was
    /// already being closed by someone else, or persistence failed — either
    /// way it stays (or remains) open for a client retry.
    net::awaitable<std::optional<repository::StoredRunSummary>>
    FinalizeAsync(const repository::ActiveRunRecord& run, repository::RunCloseReason close_reason,
                  std::span<const gameplay::workout_tracking::RunPoint> track, int64_t now_ms);

private:
    // Everything the compute phase produces in one shot off the io threads.
    struct ComputedClose
    {
        repository::StoredRunSummary summary;
        std::vector<gameplay::workout_tracking::RunPoint> valid_points;
        std::vector<repository::HexCaptureRecord> captured_hexes;
        bool captures_resolved{false};   // capture resolver actually ran
        double capture_latency_us{0.0};  // H3 resolve duration, for metrics
    };

    static repository::ValidationVerdict ClassifyVerdict(const gameplay::workout_tracking::RunMetrics& metrics,
                                                         const gameplay::workout_tracking::RunValidationConfig& config);

    /// Pure CPU: anticheat replay + capture resolve + validation + scoring.
    /// No IO inside. Runs on the WorkContext pool (work_context may be null
    /// in unit tests — then it executes inline on the caller's thread).
    ComputedClose ComputeClose(const repository::ActiveRunRecord& run, repository::RunCloseReason close_reason,
                               std::span<const gameplay::workout_tracking::RunPoint> track, int64_t now_ms) const;

    /// Map-cache writes (grouped per zone, one pipeline per zone) + capture
    /// events via the transport. Best-effort: problems are logged, the run
    /// stays closed.
    net::awaitable<void> FanOutCapturesAsync(const repository::ActiveRunRecord& run,
                                             const repository::StoredRunSummary& summary,
                                             const std::vector<repository::HexCaptureRecord>& captures,
                                             int64_t now_ms);

    std::shared_ptr<repository::IActiveRunStore> m_run_store;
    std::shared_ptr<repository::IRunHistoryRepository> m_run_history;
    std::shared_ptr<repository::IHexMapCache> m_hex_cache;
    std::shared_ptr<ICaptureEventPublisher> m_capture_publisher;
    std::shared_ptr<repository::IUserRepository> m_users;
    context::WorkContext* m_work_context{nullptr};

    gameplay::anticheat::GpsAntiCheat m_anticheat;
    gameplay::territory_conquest::H3Service m_h3_service; // outlives m_capture_resolver
    std::unique_ptr<gameplay::territory_conquest::H3CaptureResolver> m_capture_resolver;
    gameplay::workout_tracking::RunValidator m_run_validator;
};

} // namespace run::service::running
