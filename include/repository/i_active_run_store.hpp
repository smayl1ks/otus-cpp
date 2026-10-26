#pragma once

#include "repository/run_records.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace run::repository
{

// Live run state plane (Redis in production).
// Keyed by client session_id everywhere: it is present in every contract
// message, while server_run_id only travels inside the records/responses.
//
// Implementations must be safe to call from coroutines on the node's
// io_context; statelessness of the node comes for free — everything here
// lives in Redis, never in process memory.
class IActiveRunStore
{
public:
    virtual ~IActiveRunStore() = default;

    // ---- Lifecycle -------------------------------------------------------

    /// Atomic claim of the "one active run per user" slot. false = user already busy.
    virtual boost::asio::awaitable<bool> TryClaimUserSlotAsync(int64_t user_id, const std::string& session_id) = 0;

    /// Releases the slot (only if it still belongs to session_id).
    virtual boost::asio::awaitable<void> ReleaseUserSlotAsync(int64_t user_id, const std::string& session_id) = 0;

    /// Creates the active-run record after the history row exists.
    virtual boost::asio::awaitable<bool> InitActiveRunAsync(const ActiveRunRecord& record) = 0;

    virtual boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunAsync(const std::string& session_id) = 0;

    virtual boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunByUserAsync(int64_t user_id) = 0;

    // ---- Telemetry ingestion ----------------------------------------------

    /// Batch dedup (SET NX run:batch:{session_id}:{batch_uuid}). false = duplicate.
    virtual boost::asio::awaitable<bool> ClaimBatchAsync(const std::string& session_id,
                                                         const std::string& batch_uuid) = 0;

    /// Appends validated points, adds delta distance and advances last_sequence_number.
    virtual boost::asio::awaitable<bool> AppendTrackBatchAsync(const std::string& session_id,
                                                               std::span<const gameplay::workout_tracking::RunPoint>
                                                                   points,
                                                               double delta_meters, int64_t sequence_number) = 0;

    /// Full stored track, used by the finalizer.
    virtual boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    GetTrackAsync(const std::string& session_id) = 0;

    // ---- Idempotent close --------------------------------------------------

    virtual boost::asio::awaitable<bool> SaveRunSummaryAsync(const StoredRunSummary& summary) = 0;

    virtual boost::asio::awaitable<std::optional<StoredRunSummary>> GetRunSummaryAsync(const std::string& session_id) = 0;

    // ---- Offline upload assembly -------------------------------------------

    virtual boost::asio::awaitable<bool> InitUploadAsync(const UploadMetaRecord& meta) = 0;

    virtual boost::asio::awaitable<std::optional<UploadMetaRecord>> GetUploadMetaAsync(const std::string& session_id) = 0;

    /// Stores one chunk (dedup by chunk_index), returns the count of DISTINCT held chunks.
    virtual boost::asio::awaitable<int32_t> PutUploadChunkAsync(const std::string& session_id, int32_t chunk_index,
                                                                std::span<const gameplay::workout_tracking::RunPoint>
                                                                    points) = 0;

    virtual boost::asio::awaitable<std::vector<int32_t>> GetAcceptedChunksAsync(const std::string& session_id) = 0;

    /// Concatenates held chunks in chunk_index order.
    virtual boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    AssembleUploadTrackAsync(const std::string& session_id) = 0;

    /// Removes every per-run key (track, active record, upload assembly) and the user slot.
    virtual boost::asio::awaitable<void> DropRunDataAsync(const std::string& session_id, int64_t user_id) = 0;
};

} // namespace run::repository
