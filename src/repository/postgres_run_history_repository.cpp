#include "repository/postgres_run_history_repository.hpp"

#include "logger/logger.hpp"
#include "repository/connection_pool.hpp"

#include <pqxx/pqxx>

#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace run::repository
{

namespace
{

constexpr std::string_view kValidationReasonNames[] = {"NONE",
                                                       "TOO_SHORT_DISTANCE",
                                                       "TOO_SHORT_DURATION",
                                                       "TOO_LONG_DISTANCE",
                                                       "TOO_LONG_DURATION",
                                                       "ANTICHEAT_REJECTED",
                                                       "INSUFFICIENT_VALID_POINTS",
                                                       "INVALID_TIMESTAMPS"};

std::string_view ValidationReasonName(ValidationVerdict verdict)
{
    const auto index = static_cast<std::size_t>(verdict);
    return index < std::size(kValidationReasonNames) ? kValidationReasonNames[index] : "NONE";
}

/// H3 uint64 indexes are stored in BIGINT columns
constexpr int64_t ToBigInt(uint64_t h3_index)
{
    return static_cast<int64_t>(h3_index);
}

/// The run row to finalize is not in the 'active' state anymore
class RunNotActiveError final : public std::runtime_error
{
public:
    RunNotActiveError() : std::runtime_error("run is not active anymore")
    {
    }
};

/// Close the runs row: status, aggregates, validation verdict.
/// @throws RunNotActiveError if 0 or more than 1 row was affected
void FinishRunRow(pqxx::work& txn, const StoredRunSummary& summary)
{
    const auto result =
        txn.exec("UPDATE runs SET status = $2, total_distance_meters = $3, total_duration_seconds = $4, "
                 "    avg_pace_sec_per_km = $5, hexagons_captured = $6, game_points_earned = $7, "
                 "    is_valid = $8, validation_reason = $9, finished_at = to_timestamp($10 / 1000.0) "
                 "WHERE id = $1 AND status = 'active'",
                 pqxx::params{summary.server_run_id,
                              summary.close_reason == RunCloseReason::kCancelled ? "cancelled" : "finished",
                              summary.total_distance_meters, static_cast<int64_t>(summary.duration_seconds),
                              static_cast<int32_t>(summary.avg_pace_sec_per_km),
                              static_cast<int32_t>(summary.hexagons_captured_count),
                              static_cast<int32_t>(summary.total_score), summary.is_valid,
                              ValidationReasonName(summary.verdict), static_cast<int64_t>(summary.finished_at_ms)});

    if (result.affected_rows() != 1)
    {
        throw RunNotActiveError{};
    }
}

void RewriteTrackPoints(pqxx::work& txn, int64_t run_id, std::span<const gameplay::workout_tracking::RunPoint> track)
{
    txn.exec("DELETE FROM run_points WHERE run_id = $1", pqxx::params{run_id});

    if (track.empty())
    {
        return;
    }

    const std::size_t count = track.size();
    std::vector<int32_t> point_order;
    std::vector<double> latitudes;
    std::vector<double> longitudes;
    std::vector<std::optional<float>> altitudes;
    std::vector<std::optional<float>> speeds;
    std::vector<float> accuracies;
    std::vector<int64_t> h3_indexes;
    std::vector<std::optional<int32_t>> step_counts;
    std::vector<int64_t> recorded_ms;
    point_order.reserve(count);
    latitudes.reserve(count);
    longitudes.reserve(count);
    altitudes.reserve(count);
    speeds.reserve(count);
    accuracies.reserve(count);
    h3_indexes.reserve(count);
    step_counts.reserve(count);
    recorded_ms.reserve(count);

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& point = track[i];
        point_order.push_back(static_cast<int32_t>(i));
        latitudes.push_back(point.coord.latitude);
        longitudes.push_back(point.coord.longitude);
        altitudes.push_back(point.altitude_meters);
        speeds.push_back(point.speed_ms);
        accuracies.push_back(point.accuracy_meters);
        h3_indexes.push_back(ToBigInt(point.h3_index));
        step_counts.push_back(point.step_count);
        recorded_ms.push_back(point.RecordedAtMs());
    }

    // Column types are pinned by the ::[] casts; optional fields without a value arrive as SQL NULL
    txn.exec("INSERT INTO run_points (run_id, point_order, latitude, longitude, altitude, speed, "
             "                        accuracy, h3_index, step_count, recorded_at) "
             "SELECT $1::bigint, o.point_order, o.latitude, o.longitude, o.altitude, o.speed, o.accuracy, "
             "       o.h3_index, o.step_count, to_timestamp(o.recorded_ms / 1000.0) "
             "FROM UNNEST($2::int[], $3::float8[], $4::float8[], $5::real[], $6::real[], $7::real[], "
             "            $8::bigint[], $9::int[], $10::bigint[]) "
             "AS o (point_order, latitude, longitude, altitude, speed, accuracy, h3_index, step_count, recorded_ms)",
             pqxx::params{run_id, point_order, latitudes, longitudes, altitudes, speeds, accuracies, h3_indexes,
                          step_counts, recorded_ms});
}

void TransferHexOwnership(pqxx::work& txn, const StoredRunSummary& summary,
                          std::vector<HexCaptureRecord>* captured_hexes_out)
{
    if (!summary.is_valid)
    {
        return;
    }

    // Per-hex leaderboard (GET /api/v1/hexagons/{id}/details -> top_runners):
    // the capturing user earns the run's per-hex share on every captured hex.
    // For a valid run total_score == hexagons_captured_count * kPointsPerHex,
    // so the share equals the per-hex game constant
    const int32_t per_hex_points = summary.hexagons_captured_count > 0
                                       ? summary.total_score / static_cast<int32_t>(summary.hexagons_captured_count)
                                       : 0;
    const double per_hex_distance =
        summary.hexagons_captured_count > 0
            ? summary.total_distance_meters / static_cast<double>(summary.hexagons_captured_count)
            : 0.0;

    for (const uint64_t hex : summary.claimed_hexes)
    {
        const auto previous = txn.exec("SELECT owner_user_id, owner_team_id FROM hexagons WHERE h3_index = $1",
                                       pqxx::params{ToBigInt(hex)});

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

        // RETURNING carries the authoritative post-capture record so the map
        // cache can mirror it instead of a per-capture constant
        const auto upsert = txn.exec("INSERT INTO hexagons (h3_index, owner_user_id, owner_team_id, top_score, "
                                     "total_captures, captured_at, updated_at) "
                                     "VALUES ($1, $2, NULL, $3, 1, now(), now()) "
                                     "ON CONFLICT (h3_index) DO UPDATE SET "
                                     "  owner_user_id = $2, top_score = GREATEST(hexagons.top_score, $3), "
                                     "  total_captures = hexagons.total_captures + 1, captured_at = now(), "
                                     "  updated_at = now() "
                                     "RETURNING top_score",
                                     pqxx::params{ToBigInt(hex), static_cast<int64_t>(summary.user_id),
                                                  static_cast<int32_t>(summary.total_score)});
        const int32_t new_top_score = upsert.empty() ? 0 : upsert[0][0].as<int32_t>();

        if (captured_hexes_out != nullptr)
        {
            captured_hexes_out->push_back(HexCaptureRecord{hex, previous_owner, new_top_score});
        }

        txn.exec("INSERT INTO hexagon_history (h3_index, previous_owner_id, previous_team_id, new_owner_id, "
                 "new_team_id, score_at_capture, captured_at) "
                 "VALUES ($1, $2, $3, $4, NULL, $5, now())",
                 pqxx::params{ToBigInt(hex), previous_owner, previous_team, static_cast<int64_t>(summary.user_id),
                              static_cast<int32_t>(summary.total_score)});

        txn.exec("INSERT INTO hexagon_user_stats (h3_index, user_id, game_points, total_distance_meters, "
                 "visits_count, last_visited_at) "
                 "VALUES ($1, $2, $3, $4, 1, now()) "
                 "ON CONFLICT (h3_index, user_id) DO UPDATE SET "
                 "  game_points = hexagon_user_stats.game_points + $3, "
                 "  total_distance_meters = hexagon_user_stats.total_distance_meters + $4, "
                 "  visits_count = hexagon_user_stats.visits_count + 1, last_visited_at = now()",
                 pqxx::params{ToBigInt(hex), static_cast<int64_t>(summary.user_id), per_hex_points, per_hex_distance});
    }
}

/// Add the finished run's aggregates to the runner profile
void AccumulateUserStats(pqxx::work& txn, const StoredRunSummary& summary)
{
    txn.exec("INSERT INTO user_stats (user_id, total_distance_meters, total_duration_seconds, total_runs, "
             "total_game_points, current_held_hexagons, updated_at) "
             "VALUES ($1, $2, $3, CASE WHEN $4 THEN 1 ELSE 0 END, $5, 0, now()) "
             "ON CONFLICT (user_id) DO UPDATE SET "
             "  total_distance_meters = user_stats.total_distance_meters + $2, "
             "  total_duration_seconds = user_stats.total_duration_seconds + $3, "
             "  total_runs = user_stats.total_runs + CASE WHEN $4 THEN 1 ELSE 0 END, "
             "  total_game_points = user_stats.total_game_points + $5, "
             "  current_held_hexagons = (SELECT count(*) FROM hexagons WHERE owner_user_id = $1), "
             "  updated_at = now()",
             pqxx::params{static_cast<int64_t>(summary.user_id), summary.total_distance_meters,
                          static_cast<int64_t>(summary.duration_seconds), summary.is_valid,
                          static_cast<int32_t>(summary.total_score)});
}

} // namespace

PostgresRunHistoryRepository::PostgresRunHistoryRepository(run::context::WorkContext& work_context)
    : m_work_context(work_context)
{
}

boost::asio::awaitable<std::optional<int64_t>> PostgresRunHistoryRepository::CreateRunAsync(const NewRunRecord& record)
{
    co_return co_await m_work_context.AsyncPost(
        [record]() -> std::optional<int64_t>
        {
            try
            {
                auto connection = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection);

                // team_id/event_id stay out of the runs schema for now:
                // they live in the active-run record (Redis plane)
                const auto result = txn.exec("INSERT INTO runs (user_id, session_id, status, started_at) "
                                             "VALUES ($1, $2, 'active', to_timestamp($3 / 1000.0)) "
                                             "RETURNING id",
                                             pqxx::params{static_cast<int64_t>(record.user_id), record.session_id,
                                                          static_cast<int64_t>(record.started_at_ms)});

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
                                               std::span<const gameplay::workout_tracking::RunPoint> track,
                                               std::vector<HexCaptureRecord>* captured_hexes_out)
{
    auto captured_hexes = std::make_shared<std::vector<HexCaptureRecord>>();
    const bool persisted = co_await m_work_context.AsyncPost(
        [summary, track, captured_hexes]() -> bool
        {
            try
            {
                auto connection = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection);

                FinishRunRow(txn, summary);
                RewriteTrackPoints(txn, summary.server_run_id, track);
                TransferHexOwnership(txn, summary, captured_hexes.get());
                AccumulateUserStats(txn, summary);

                txn.commit();

                LOG_INFO("Run {} ('{}') persisted: {} points, {} hexes, valid={}", summary.server_run_id,
                         summary.session_id, track.size(), summary.hexagons_captured_count, summary.is_valid);
                return true;
            }
            catch (const RunNotActiveError&)
            {
                LOG_WARN("Run {} ('{}') is not active anymore — finalize refused", summary.server_run_id,
                         summary.session_id);
                captured_hexes->clear();
                return false;
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to finalize run {} ('{}'): {}", summary.server_run_id, summary.session_id, e.what());
                captured_hexes->clear();
                return false;
            }
        });

    if (persisted && captured_hexes_out != nullptr)
    {
        *captured_hexes_out = std::move(*captured_hexes);
    }
    co_return persisted;
}

} // namespace run::repository
