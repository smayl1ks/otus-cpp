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
};

/// PostgreSQL/PostGIS history repository interface managing run lifecycles and atomic finalization
class IRunHistoryRepository
{
public:
    virtual ~IRunHistoryRepository() = default;

    /// Inserts an active run record and returns the generated server run ID
    virtual boost::asio::awaitable<std::optional<int64_t>> CreateRunAsync(const NewRunRecord& record) = 0;

    /// Atomic final transaction for run closure (updates state, track points, hex captures, and user stats)
    virtual boost::asio::awaitable<bool>
    FinalizeRunAsync(const StoredRunSummary& summary, std::span<const gameplay::workout_tracking::RunPoint> track,
                     std::vector<HexCaptureRecord>* captured_hexes_out = nullptr) = 0;
};

} // namespace run::repository
