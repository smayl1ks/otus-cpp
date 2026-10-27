#include "repository/postgres_run_history_repository.hpp"

#include "logger/logger.hpp"
#include "repository/connection_pool.hpp"

#include <fmt/format.h>
#include <pqxx/pqxx>

#include <algorithm>

namespace run::repository
{

namespace
{

constexpr const char* kValidationReasonNames[] = {
    "NONE",           "TOO_SHORT_DISTANCE", "TOO_SHORT_DURATION", "TOO_LONG_DISTANCE", "TOO_LONG_DURATION",
    "ANTICHEAT_REJECTED", "INSUFFICIENT_VALID_POINTS", "INVALID_TIMESTAMPS"};

std::string_view ValidationReasonName(ValidationVerdict verdict)
{
    const auto index = static_cast<std::size_t>(verdict);
    return index < std::size(kValidationReasonNames) ? kValidationReasonNames[index] : "NONE";
}

/// H3 uint64 indexes are stored in BIGINT columns: reinterpret, never truncate.
constexpr int64_t ToBigInt(uint64_t h3_index)
{
    return static_cast<int64_t>(h3_index);
}

} // namespace

PostgresRunHistoryRepository::PostgresRunHistoryRepository(run::context::WorkContext& work_context)
    : m_work_context(work_context)
{
}

boost::asio::awaitable<std::optional<int64_t>>
PostgresRunHistoryRepository::CreateRunAsync(const NewRunRecord& record)
{
    co_return co_await m_work_context.AsyncPost(
        [record]() -> std::optional<int64_t>
        {
            try
            {
                auto connection = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection);

                // team_id/event_id stay out of the runs schema for now:
                // they live in the active-run record (Redis plane).
                auto result = txn.exec_params(
                    "INSERT INTO runs (user_id, session_id, status, started_at) "
                    "VALUES ($1, $2, 'active', to_timestamp($3 / 1000.0)) "
                    "RETURNING id",
                    static_cast<int64_t>(record.user_id), record.session_id,
                    static_cast<int64_t>(record.started_at_ms));

                txn.commit();

                if (result.empty())
                {
                    return std::nullopt;
                }
                return result[0][0].as<int64_t>();
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to create run row for session '{}': {}", record.session_id, e.what());
                return std::nullopt;
            }
        });
}

boost::asio::awaitable<bool>
PostgresRunHistoryRepository::FinalizeRunAsync(const StoredRunSummary& summary,
                                               std::span<const gameplay::workout_tracking::RunPoint> track)
{
    co_return co_await m_work_context.AsyncPost(
        [summary, track]() -> bool
        {
            try
            {
                auto connection = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection);

                // [1] Runs row: final state. Invalid runs are stored too
                // (history survives, nothing is scored).
                txn.exec_params(
                    "UPDATE runs SET status = $2, total_distance_meters = $3, total_duration_seconds = $4, "
                    "    avg_pace_sec_per_km = $5, hexagons_captured = $6, game_points_earned = $7, "
                    "    is_valid = $8, validation_reason = $9, finished_at = to_timestamp($10 / 1000.0) "
                    "WHERE id = $1",
                    summary.server_run_id,
                    summary.close_reason == RunCloseReason::kCancelled ? "cancelled" : "finished",
                    summary.total_distance_meters, static_cast<int64_t>(summary.duration_seconds),
                    static_cast<int32_t>(summary.avg_pace_sec_per_km),
                    static_cast<int32_t>(summary.hexagons_captured_count),
                    static_cast<int32_t>(summary.total_score), summary.is_valid,
                    std::string(ValidationReasonName(summary.verdict)),
                    static_cast<int64_t>(summary.finished_at_ms));

                // [2] Track points: the track was assembled in the active-run
                // store, PostgreSQL is its first durable copy.
                txn.exec_params("DELETE FROM run_points WHERE run_id = $1",
                                summary.server_run_id);
                constexpr std::size_t kBatchSize = 500;
                constexpr int kFieldsPerPoint = 10;
                for (std::size_t offset = 0; offset < track.size(); offset += kBatchSize)
                {
                    const std::size_t count = std::min(kBatchSize, track.size() - offset);
                    pqxx::params batch;
                    std::string sql =
                        "INSERT INTO run_points (run_id, point_order, latitude, longitude, altitude, speed, "
                        "accuracy, h3_index, step_count, recorded_at) VALUES ";

                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto& point = track[offset + i];
                        if (i > 0)
                        {
                            sql += ", ";
                        }
                        const int base = static_cast<int>(i) * kFieldsPerPoint;
                        fmt::format_to(std::back_inserter(sql),
                                       "(${},${},${},${},${},${},${},${},${},to_timestamp(${} / 1000.0))", base + 1,
                                       base + 2, base + 3, base + 4, base + 5, base + 6, base + 7, base + 8,
                                       base + 9, base + 10);

                        batch.append(static_cast<int64_t>(summary.server_run_id));
                        batch.append(static_cast<int32_t>(offset + i));
                        batch.append(point.coord.latitude);
                        batch.append(point.coord.longitude);
                        batch.append(point.altitude_meters.has_value() ? std::make_optional(*point.altitude_meters)
                                                                       : std::nullopt);
                        batch.append(point.speed_ms.has_value() ? std::make_optional(*point.speed_ms)
                                                                : std::nullopt);
                        batch.append(point.accuracy_meters);
                        batch.append(ToBigInt(point.h3_index));
                        batch.append(point.step_count); // optional<int32_t>: null when unset
                        batch.append(static_cast<int64_t>(point.RecordedAtMs()));
                    }

                    txn.exec_params(sql, batch);
                }

                // [3] Territory: upsert ownership, keep history, reinforce score.
                for (const uint64_t hex : summary.claimed_hexes)
                {
                    const auto previous =
                        txn.exec_params("SELECT owner_user_id, owner_team_id FROM hexagons WHERE h3_index = $1",
                                        ToBigInt(hex));

                    std::optional<int64_t> previous_owner;
                    std::optional<int64_t> previous_team;
                    if (!previous.empty())
                    {
                        if (!previous[0][0].is_null())
                        {
                            previous_owner = previous[0][0].as<int64_t>();
                        }
                        if (!previous[0][1].is_null())
                        {
                            previous_team = previous[0][1].as<int64_t>();
                        }
                    }

                    txn.exec_params(
                        "INSERT INTO hexagons (h3_index, owner_user_id, owner_team_id, top_score, total_captures, "
                        "captured_at, updated_at) "
                        "VALUES ($1, $2, NULL, $3, 1, now(), now()) "
                        "ON CONFLICT (h3_index) DO UPDATE SET "
                        "  owner_user_id = $2, top_score = GREATEST(hexagons.top_score, $3), "
                        "  total_captures = hexagons.total_captures + 1, captured_at = now(), updated_at = now()",
                        ToBigInt(hex), static_cast<int64_t>(summary.user_id),
                        static_cast<int32_t>(summary.total_score));

                    pqxx::params history;
                    history.append(ToBigInt(hex));
                    history.append(previous_owner); // std::optional: null when hex had no owner
                    history.append(previous_team);
                    history.append(static_cast<int64_t>(summary.user_id));
                    history.append(static_cast<int32_t>(summary.total_score));
                    txn.exec_params(
                        "INSERT INTO hexagon_history (h3_index, previous_owner_id, previous_team_id, new_owner_id, "
                        "new_team_id, score_at_capture, captured_at) "
                        "VALUES ($1, $2, $3, $4, NULL, $5, now())",
                        history);
                }

                // [4] Aggregates for the runner profile.
                txn.exec_params(
                    "INSERT INTO user_stats (user_id, total_distance_meters, total_duration_seconds, total_runs, "
                    "total_game_points, current_held_hexagons, updated_at) "
                    "VALUES ($1, $2, $3, CASE WHEN $4 THEN 1 ELSE 0 END, $5, 0, now()) "
                    "ON CONFLICT (user_id) DO UPDATE SET "
                    "  total_distance_meters = user_stats.total_distance_meters + $2, "
                    "  total_duration_seconds = user_stats.total_duration_seconds + $3, "
                    "  total_runs = user_stats.total_runs + CASE WHEN $4 THEN 1 ELSE 0 END, "
                    "  total_game_points = user_stats.total_game_points + $5, "
                    "  current_held_hexagons = (SELECT count(*) FROM hexagons WHERE owner_user_id = $1), "
                    "  updated_at = now()",
                    static_cast<int64_t>(summary.user_id), summary.total_distance_meters,
                    static_cast<int64_t>(summary.duration_seconds), summary.is_valid,
                    static_cast<int32_t>(summary.total_score));

                txn.commit();

                LOG_INFO("Run {} ('{}') persisted: {} points, {} hexes, valid={}", summary.server_run_id,
                         summary.session_id, track.size(), summary.hexagons_captured_count, summary.is_valid);
                return true;
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to finalize run {} ('{}'): {}", summary.server_run_id, summary.session_id, e.what());
                return false;
            }
        });
}

} // namespace run::repository
