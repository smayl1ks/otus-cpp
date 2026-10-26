#pragma once

#include "repository/run_records.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <span>

namespace run::repository
{

// History persistence plane (PostgreSQL/PostGIS in production).
// Writes happen only at two points of the lifecycle:
//   * CreateRunAsync      — at StartRun / OfflineRunInit (runs row, status='active');
//   * FinalizeRunAsync    — at FinishRun / CancelRun / OfflineRunCommit, one
//     transaction covering the runs row, run_points batch, hexagon upserts,
//     hexagon_history and user_stats.
class IRunHistoryRepository
{
public:
    virtual ~IRunHistoryRepository() = default;

    /// Inserts runs(status='active'), returns the generated server_run_id.
    virtual boost::asio::awaitable<std::optional<int64_t>> CreateRunAsync(const NewRunRecord& record) = 0;

    /// Final transaction of a closing run: final row state + track + captures + stats.
    virtual boost::asio::awaitable<bool> FinalizeRunAsync(const StoredRunSummary& summary,
                                                          std::span<const gameplay::workout_tracking::RunPoint> track) = 0;
};

} // namespace run::repository
