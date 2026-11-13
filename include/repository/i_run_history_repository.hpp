#pragma once

#include "gameplay/workout_tracking/workout_session.hpp"
#include "repository/run_records.hpp"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <span>

namespace run::repository
{

/// Represents a newly captured hex with its H3 index and previous owner (if any)
struct HexCaptureRecord
{
    uint64_t h3_index{0};
    std::optional<int64_t> prev_owner_id;
    /// Authoritative hex record after the capture (hexagons.top_score =
    /// GREATEST(previous, run total)), read back via RETURNING
    int32_t top_score{0};
};

/// Outcome of the PostgreSQL finalize transaction
enum class FinalizeStatus
{
    kPersisted,
    kAlreadyFinished,
    kError,
};

/// PostgreSQL/PostGIS history repository interface managing run lifecycles and atomic finalization
class IRunHistoryRepository
{
public:
    virtual ~IRunHistoryRepository() = default;

    /// Inserts an active run record and returns the generated server run ID
    virtual boost::asio::awaitable<std::optional<int64_t>> CreateRunAsync(const NewRunRecord& record) = 0;

    /// Atomic final transaction for run closure (updates state, track points, hex captures, and user stats)
    virtual boost::asio::awaitable<FinalizeStatus>
    FinalizeRunAsync(const StoredRunSummary& summary, std::span<const gameplay::workout_tracking::RunPoint> track,
                     std::vector<HexCaptureRecord>* captured_hexes_out = nullptr) = 0;

    /// Rebuilds the stored result of an already finalized run from PostgreSQL
    /// nullopt = no finalized run for this session/user
    virtual boost::asio::awaitable<std::optional<StoredRunSummary>> GetFinalizedRunAsync(const std::string& session_id,
                                                                                         int64_t user_id) = 0;
};

} // namespace run::repository
