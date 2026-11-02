#pragma once

#include "repository/run_records.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <span>

namespace run::repository
{

// One newly captured hex: its res-9 index and the owner it was taken from
// (nullopt when the hex had no owner). Filled by FinalizeRunAsync.
struct HexCaptureRecord
{
    uint64_t h3_index{0};
    std::optional<int64_t> prev_owner_id;
};

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
    /// When captured_hexes_out is not null it receives one record per claimed hex,
    /// with the previous owner as observed inside this very transaction — the
    /// source of truth for HexagonCaptureEvent fan-out.
    virtual boost::asio::awaitable<bool>
    FinalizeRunAsync(const StoredRunSummary& summary, std::span<const gameplay::workout_tracking::RunPoint> track,
                     std::vector<HexCaptureRecord>* captured_hexes_out = nullptr) = 0;
};

} // namespace run::repository
