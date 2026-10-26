#include "service/running/run_finalizer.hpp"
#include "monitoring/metrics_collector.hpp"
#include <chrono>

#include "logger/logger.hpp"

#include <algorithm>
#include <chrono>

namespace run::service::running
{

namespace
{

using gameplay::anticheat::TelemetryPoint;
using gameplay::workout_tracking::RunMetrics;
using gameplay::workout_tracking::RunPoint;

// Game balance placeholder: points credited per newly captured hex.
constexpr uint32_t kPointsPerHex{10};

TelemetryPoint ToTelemetry(const RunPoint& point)
{
    return TelemetryPoint::Create(point.coord.latitude, point.coord.longitude, point.RecordedAtMs(),
                                  point.accuracy_meters, /*is_mock=*/false, point.speed_ms, point.altitude_meters,
                                  point.heading_degrees);
}

} // namespace

RunFinalizer::RunFinalizer(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                           std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                           gameplay::anticheat::GpsAntiCheatConfig anticheat_config,
                           gameplay::territory_conquest::H3CaptureConfig h3_config,
                           gameplay::workout_tracking::RunValidationConfig validation_config)
    : m_store(std::move(active_run_store)), m_history(std::move(history_repository)), m_anticheat(anticheat_config),
      m_capture_resolver(std::make_unique<gameplay::territory_conquest::H3CaptureResolver>(m_h3_service, h3_config)),
      m_run_validator(validation_config)
{
}

repository::ValidationVerdict
RunFinalizer::ClassifyVerdict(const RunMetrics& metrics, const gameplay::workout_tracking::RunValidationConfig& config)
{
    if (metrics.total_distance_meters < config.min_distance_meters)
    {
        return repository::ValidationVerdict::kTooShortDistance;
    }
    if (metrics.duration_seconds < config.min_duration_seconds)
    {
        return repository::ValidationVerdict::kTooShortDuration;
    }
    if (metrics.total_distance_meters > config.max_distance_meters)
    {
        return repository::ValidationVerdict::kTooLongDistance;
    }
    if (metrics.duration_seconds > config.max_duration_seconds)
    {
        return repository::ValidationVerdict::kTooLongDuration;
    }
    return repository::ValidationVerdict::kValid;
}

boost::asio::awaitable<std::optional<repository::StoredRunSummary>>
RunFinalizer::FinalizeAsync(const repository::ActiveRunRecord& run, repository::RunCloseReason close_reason,
                            std::span<const RunPoint> track, int64_t now_ms)
{
    LOG_INFO("[finalizer] closing session '{}' (user {}, reason: {}, track points: {})", run.session_id, run.user_id,
             close_reason == repository::RunCloseReason::kFinished ? "finished" : "cancelled", track.size());

    repository::StoredRunSummary summary;
    summary.session_id = run.session_id;
    summary.server_run_id = run.server_run_id;
    summary.user_id = run.user_id;
    summary.close_reason = close_reason;
    summary.started_at_ms = run.started_at_ms;
    summary.finished_at_ms = now_ms;

    // [1] Anti-cheat replay point-by-point; a finished track may contain
    // points whose neighboring batches were never re-validated together.
    std::vector<RunPoint> valid_points;
    valid_points.reserve(track.size());
    int64_t first_ts_ms = 0;
    int64_t last_ts_ms = 0;
    for (const auto& point : track)
    {
        if (m_anticheat.ValidatePoint(ToTelemetry(point), now_ms) != gameplay::anticheat::AnticheatStatus::Ok)
        {
            continue;
        }
        if (valid_points.empty())
        {
            first_ts_ms = point.RecordedAtMs();
        }
        last_ts_ms = point.RecordedAtMs();
        valid_points.push_back(point);
    }

    if (valid_points.size() < 2)
    {
        summary.verdict = repository::ValidationVerdict::kInsufficientValidPoints;
        summary.is_valid = false;
    }

    // [2] Territory capture, once over the whole validated track. Cancelled
    // runs keep their track but capture nothing.
    gameplay::territory_conquest::CaptureResult captures;
    if (close_reason == repository::RunCloseReason::kFinished && valid_points.size() >= 2)
    {
        auto t0 = std::chrono::steady_clock::now();
        captures = m_capture_resolver->Resolve(std::span<const RunPoint>(valid_points));
        auto t1 = std::chrono::steady_clock::now();
        double latency_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        summary.claimed_hexes = captures.claimed_hexes;
        summary.hexagons_captured_count = static_cast<uint32_t>(captures.claimed_hexes.size());

        monitoring::MetricsCollector::Instance().IncrementResolvedTracks();
        monitoring::MetricsCollector::Instance().AddCapturedCells(static_cast<double>(captures.claimed_hexes.size()));
        monitoring::MetricsCollector::Instance().ObserveLatency(latency_us);
    }

    // [3] Validation thresholds. Distance comes from the capture-resolved
    // track; duration from the point timeline (client timestamps stay
    // untrusted: declared start/finish are only cross-checked upstream).
    RunMetrics metrics;
    bool thresholds_valid = true;
    if (valid_points.size() >= 2)
    {
        metrics = m_run_validator.Validate(std::span<const RunPoint>(valid_points));
        summary.total_distance_meters =
            captures.total_distance_meters > 0.0 ? captures.total_distance_meters : metrics.total_distance_meters;
        summary.duration_seconds = static_cast<uint32_t>(metrics.duration_seconds);
        thresholds_valid = metrics.is_valid;
    }
    if (summary.duration_seconds == 0 && last_ts_ms > first_ts_ms)
    {
        summary.duration_seconds = static_cast<uint32_t>((last_ts_ms - first_ts_ms) / 1000);
    }
    if (summary.is_valid && !thresholds_valid)
    {
        summary.verdict = ClassifyVerdict(metrics, m_run_validator.GetConfig());
        summary.is_valid = false;
    }
    if (close_reason == repository::RunCloseReason::kCancelled)
    {
        // Cancelled runs are stored but never scored.
        summary.is_valid = false;
    }

    // [4] Derived stats. Moving duration / pace refinement lands with the
    // pause-aware metrics; placeholders keep the summary shape complete.
    summary.total_points_count = static_cast<int32_t>(valid_points.size());
    summary.moving_duration_seconds = summary.duration_seconds;
    summary.avg_pace_sec_per_km = summary.total_distance_meters > 1.0
                                      ? static_cast<uint32_t>(static_cast<double>(summary.duration_seconds) /
                                                              (summary.total_distance_meters / 1000.0))
                                      : 0;
    summary.avg_speed_kmh =
        summary.duration_seconds > 0
            ? static_cast<float>(summary.total_distance_meters / static_cast<double>(summary.duration_seconds) * 3.6)
            : 0.0f;
    summary.total_score = summary.is_valid ? summary.hexagons_captured_count * kPointsPerHex : 0;

    // [5] PostgreSQL final transaction: runs row + points + captures + stats.
    if (!(co_await m_history->FinalizeRunAsync(summary, valid_points)))
    {
        LOG_ERROR("[finalizer] history persistence failed for session '{}', run stays open", run.session_id);
        co_return std::nullopt;
    }

    // [6] Redis cleanup: summary for idempotent retries, then every per-run
    // key plus the user slot.
    co_await m_store->SaveRunSummaryAsync(summary);
    co_await m_store->DropRunDataAsync(run.session_id, run.user_id);

    LOG_INFO("[finalizer] session '{}' closed: {} m, {} s, {} hexes, valid={}", run.session_id,
             summary.total_distance_meters, summary.duration_seconds, summary.hexagons_captured_count,
             summary.is_valid);
    co_return summary;
}

} // namespace run::service::running
