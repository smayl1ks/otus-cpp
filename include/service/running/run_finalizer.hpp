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

/// Notice of a territory hex changing hands at run close
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

/// Transport interface for publishing territory capture events
struct ICaptureEventPublisher
{
    virtual ~ICaptureEventPublisher() = default;

    virtual net::awaitable<void> PublishAsync(const CaptureNotice& notice) = 0;
};

/// Unified run finalizer for Finish, Cancel, and Offline Uploads (handles claim, compute,
/// persist, fan-out, and cleanup phases)
class RunFinalizer
{
public:
    RunFinalizer(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                 std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                 context::WorkContext* work_context, gameplay::anticheat::GpsAntiCheatConfig anticheat_config,
                 gameplay::territory_conquest::H3CaptureConfig h3_config,
                 gameplay::workout_tracking::RunValidationConfig validation_config,
                 std::shared_ptr<repository::IHexMapCache> hex_map_cache,
                 std::shared_ptr<ICaptureEventPublisher> capture_publisher,
                 std::shared_ptr<repository::IUserRepository> user_repository);

    /// Finalizes the run. Returns std::nullopt if already closing or persistence failed
    net::awaitable<std::optional<repository::StoredRunSummary>>
    FinalizeAsync(const repository::ActiveRunRecord& run, repository::RunCloseReason close_reason,
                  std::span<const gameplay::workout_tracking::RunPoint> track, int64_t now_ms);

private:
    struct ComputedClose
    {
        repository::StoredRunSummary summary;
        std::vector<gameplay::workout_tracking::RunPoint> valid_points;
        std::vector<repository::HexCaptureRecord> captured_hexes;
        bool captures_resolved{false};
        double capture_latency_us{0.0};
    };

    static repository::ValidationVerdict ClassifyVerdict(const gameplay::workout_tracking::RunMetrics& metrics,
                                                         const gameplay::workout_tracking::RunValidationConfig& config);

    /// CPU-heavy phase: anticheat replay, capture resolution, validation, and scoring
    ComputedClose ComputeClose(const repository::ActiveRunRecord& run, repository::RunCloseReason close_reason,
                               std::span<const gameplay::workout_tracking::RunPoint> track, int64_t now_ms) const;

    /// Map-cache updates and capture event publishing (best-effort)
    net::awaitable<void> FanOutCapturesAsync(const repository::ActiveRunRecord& run,
                                             const repository::StoredRunSummary& summary,
                                             const std::vector<repository::HexCaptureRecord>& captures, int64_t now_ms);

    std::shared_ptr<repository::IActiveRunStore> m_run_store;
    std::shared_ptr<repository::IRunHistoryRepository> m_run_history;
    std::shared_ptr<repository::IHexMapCache> m_hex_cache;
    std::shared_ptr<ICaptureEventPublisher> m_capture_publisher;
    std::shared_ptr<repository::IUserRepository> m_users;
    context::WorkContext* m_work_context{nullptr};

    gameplay::anticheat::GpsAntiCheat m_anticheat;
    gameplay::territory_conquest::H3Service m_h3_service;
    std::unique_ptr<gameplay::territory_conquest::H3CaptureResolver> m_capture_resolver;
    gameplay::workout_tracking::RunValidator m_run_validator;
};

} // namespace run::service::running
