#include "service/running/run_finalizer.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>

#include "gameplay/telemetry_conversion.hpp"
#include "logger/logger.hpp"
#include "monitoring/metrics_collector.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <map>

namespace run::service::running
{

namespace
{

using gameplay::anticheat::TelemetryPoint;
using gameplay::workout_tracking::RunMetrics;
using gameplay::workout_tracking::RunPoint;

constexpr uint32_t kPointsPerHex{10};

constexpr std::size_t kMinValidPoints{2};

} // namespace

RunFinalizer::RunFinalizer(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                           std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                           context::WorkContext* work_context, gameplay::anticheat::GpsAntiCheatConfig anticheat_config,
                           gameplay::territory_conquest::H3CaptureConfig h3_config,
                           gameplay::workout_tracking::RunValidationConfig validation_config,
                           std::shared_ptr<repository::IHexMapCache> hex_map_cache,
                           std::shared_ptr<ICaptureEventPublisher> capture_publisher,
                           std::shared_ptr<repository::IUserRepository> user_repository)
    : m_run_store(std::move(active_run_store)), m_run_history(std::move(history_repository)),
      m_hex_cache(std::move(hex_map_cache)), m_capture_publisher(std::move(capture_publisher)),
      m_users(std::move(user_repository)), m_work_context(work_context), m_anticheat(anticheat_config),
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

RunFinalizer::ComputedClose RunFinalizer::ComputeClose(const repository::ActiveRunRecord& run,
                                                       repository::RunCloseReason close_reason,
                                                       std::span<const RunPoint> track, int64_t now_ms) const
{
    ComputedClose result;
    repository::StoredRunSummary& summary = result.summary;
    summary.session_id = run.session_id;
    summary.server_run_id = run.server_run_id;
    summary.user_id = run.user_id;
    summary.close_reason = close_reason;
    summary.started_at_ms = run.started_at_ms;
    summary.finished_at_ms = now_ms;

    // Anticheat replay: the ingestor checks only batch seams, here every
    // consecutive pair of surviving points is re-validated
    std::vector<RunPoint>& valid_points = result.valid_points;
    valid_points.reserve(track.size());
    int64_t first_ts_ms = 0;
    int64_t last_ts_ms = 0;
    for (const auto& point : track)
    {
        if (m_anticheat.ValidatePoint(gameplay::ToTelemetryPoint(point), now_ms) !=
            gameplay::anticheat::AnticheatStatus::Ok)
        {
            continue;
        }
        if (!valid_points.empty() &&
            m_anticheat.ValidateSegment(gameplay::ToTelemetryPoint(valid_points.back()),
                                        gameplay::ToTelemetryPoint(point)) != gameplay::anticheat::AnticheatStatus::Ok)
        {
            // Teleport vs the last accepted point: drop it, keep the anchor,
            // so no distance is credited across the gap
            continue;
        }
        if (valid_points.empty())
        {
            first_ts_ms = point.RecordedAtMs();
        }
        last_ts_ms = point.RecordedAtMs();
        valid_points.push_back(point);
    }

    gameplay::territory_conquest::CaptureResult captures;
    if (valid_points.size() < kMinValidPoints)
    {
        summary.verdict = repository::ValidationVerdict::kInsufficientValidPoints;
        summary.is_valid = false;
    }
    else if (close_reason == repository::RunCloseReason::kFinished)
    {
        const auto t0 = std::chrono::steady_clock::now();
        captures = m_capture_resolver->Resolve(std::span<const RunPoint>(valid_points));
        result.capture_latency_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        result.captures_resolved = true;
    }

    result.captured_hexes.reserve(captures.claimed_hexes.size());
    for (const uint64_t hex : captures.claimed_hexes)
    {
        result.captured_hexes.push_back(repository::HexCaptureRecord{hex, std::nullopt});
    }

    // Validation thresholds. Distance comes from the capture-resolved
    // track; duration from the point timeline (client timestamps stay
    // untrusted: declared start/finish are only cross-checked upstream)
    RunMetrics metrics;
    bool thresholds_valid = true;
    if (valid_points.size() >= kMinValidPoints)
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
        // Cancelled runs are stored but never scored
        summary.is_valid = false;
    }

    summary.claimed_hexes = captures.claimed_hexes;
    summary.hexagons_captured_count = static_cast<uint32_t>(captures.claimed_hexes.size());

    if (!summary.is_valid)
    {
        summary.claimed_hexes.clear();
        summary.hexagons_captured_count = 0;
        result.captured_hexes.clear();
    }

    // Derived stats. Moving duration / pace refinement lands with the
    // pause-aware metrics; placeholders keep the summary shape complete
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

    return result;
}

net::awaitable<std::optional<repository::StoredRunSummary>>
RunFinalizer::FinalizeAsync(const repository::ActiveRunRecord& run, repository::RunCloseReason close_reason,
                            std::span<const RunPoint> track, int64_t now_ms)
{
    LOG_INFO("[finalizer] closing session '{}' (user {}, reason: {}, track points: {})", run.session_id, run.user_id,
             close_reason == repository::RunCloseReason::kFinished ? "finished" : "cancelled", track.size());

    if (!(co_await m_run_store->TryClaimClosingAsync(run.session_id)))
    {
        LOG_INFO("[finalizer] session '{}' is already being closed by someone else", run.session_id);
        co_return std::nullopt;
    }

    ComputedClose computed;
    if (m_work_context != nullptr)
    {
        computed = co_await m_work_context->AsyncPost([this, &run, close_reason, track, now_ms]
                                                      { return ComputeClose(run, close_reason, track, now_ms); });
    }
    else
    {
        computed = ComputeClose(run, close_reason, track, now_ms);
    }

    if (computed.captures_resolved)
    {
        monitoring::MetricsCollector::Instance().IncrementResolvedTracks();
        monitoring::MetricsCollector::Instance().AddCapturedCells(
            static_cast<double>(computed.summary.hexagons_captured_count));
        monitoring::MetricsCollector::Instance().ObserveLatency(computed.capture_latency_us);
    }

    repository::StoredRunSummary summary = std::move(computed.summary);

    // PostgreSQL final transaction
    const auto finalize_status =
        co_await m_run_history->FinalizeRunAsync(summary, computed.valid_points, &computed.captured_hexes);
    if (finalize_status == repository::FinalizeStatus::kError)
    {
        LOG_ERROR("[finalizer] history persistence failed for session '{}', run stays open", run.session_id);
        co_await m_run_store->ReleaseClosingAsync(run.session_id);
        co_return std::nullopt;
    }
    if (finalize_status == repository::FinalizeStatus::kAlreadyFinished)
    {
        auto recovered = co_await m_run_history->GetFinalizedRunAsync(run.session_id, run.user_id);
        if (!recovered.has_value())
        {
            LOG_ERROR("[finalizer] run '{}' already finalized but not recoverable from PostgreSQL", run.session_id);
            co_await m_run_store->ReleaseClosingAsync(run.session_id);
            co_return std::nullopt;
        }
        LOG_INFO("[finalizer] session '{}' result recovered from PostgreSQL (redis summary lost)", run.session_id);
        summary = std::move(*recovered);
        if (!(co_await m_run_store->SaveRunSummaryAsync(summary)))
        {
            LOG_ERROR("[finalizer] failed to save recovered summary for session '{}'", run.session_id);
        }
        co_await m_run_store->DropRunDataAsync(run.session_id, run.user_id);
        co_return summary;
    }

    // map cache + capture events (invalid runs carry no captures)
    if (summary.is_valid && !computed.captured_hexes.empty())
    {
        co_await FanOutCapturesAsync(run, summary, computed.captured_hexes, now_ms);
    }

    // cleanup: summary survives for idempotent retries
    if (!(co_await m_run_store->SaveRunSummaryAsync(summary)))
    {
        LOG_ERROR("[finalizer] failed to save summary marker for session '{}'", run.session_id);
    }

    co_await m_run_store->DropRunDataAsync(run.session_id, run.user_id);

    LOG_INFO("[finalizer] session '{}' closed: {} m, {} s, {} hexes, valid={}", run.session_id,
             summary.total_distance_meters, summary.duration_seconds, summary.hexagons_captured_count,
             summary.is_valid);
    co_return summary;
}

net::awaitable<void> RunFinalizer::FanOutCapturesAsync(const repository::ActiveRunRecord& run,
                                                       const repository::StoredRunSummary& summary,
                                                       const std::vector<repository::HexCaptureRecord>& captures,
                                                       int64_t now_ms)
{
    std::string username;
    std::string color;
    if (m_users)
    {
        if (auto user = co_await m_users->FindById(run.user_id); user.has_value())
        {
            username = user->username;
            color = user->player_color_hex;
        }
    }
    if (username.empty())
    {
        username = fmt::format("runner-{}", run.user_id);
    }
    if (color.empty())
    {
        color = "#000000";
    }

    // Map cache: group by parent zone, one pipeline per zone
    if (m_hex_cache)
    {
        std::map<uint64_t, std::vector<repository::HexOwnershipRecord>> per_zone;
        for (const auto& capture : captures)
        {
            if (auto parent_zone = m_h3_service.GetParent(capture.h3_index, 6); parent_zone.has_value())
            {
                per_zone[*parent_zone].push_back(repository::HexOwnershipRecord{
                    capture.h3_index, static_cast<uint64_t>(run.user_id), username, color,
                    capture.top_score != 0 ? capture.top_score : static_cast<int32_t>(kPointsPerHex)});
            }
        }
        for (const auto& [zone6, hexes] : per_zone)
        {
            if (!(co_await m_hex_cache->WriteZoneBatchAsync(hexes, zone6)))
            {
                LOG_WARN("[finalizer] map cache write failed for {} hexes of zone {:x}", hexes.size(), zone6);
            }
        }
    }

    // Capture events
    for (const auto& capture : captures)
    {
        const CaptureNotice notice{.h3_index = capture.h3_index,
                                   .new_owner_id = run.user_id,
                                   .new_owner_name = username,
                                   .new_owner_color_hex = color,
                                   .prev_owner_id = capture.prev_owner_id,
                                   .score_at_capture = static_cast<int32_t>(kPointsPerHex),
                                   .timestamp_ms = now_ms};
        if (m_capture_publisher)
        {
            auto publisher = m_capture_publisher;
            net::co_spawn(
                co_await net::this_coro::executor, [publisher, notice]() -> net::awaitable<void>
                { co_await publisher->PublishAsync(notice); }, net::detached);
        }
    }
}

} // namespace run::service::running
