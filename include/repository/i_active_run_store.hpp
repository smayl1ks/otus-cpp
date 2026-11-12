#pragma once

#include "gameplay/workout_tracking/workout_session.hpp"
#include "repository/run_records.hpp"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace run::repository
{

class IActiveRunStore
{
public:
    virtual ~IActiveRunStore() = default;

    // Lifecycle

    /// Atomic claim of the "one active run per user" slot. false = user already busy
    virtual boost::asio::awaitable<bool> TryClaimUserSlotAsync(int64_t user_id, const std::string& session_id) = 0;

    virtual boost::asio::awaitable<void> ReleaseUserSlotAsync(int64_t user_id, const std::string& session_id) = 0;

    virtual boost::asio::awaitable<bool> InitActiveRunAsync(const ActiveRunRecord& record) = 0;

    virtual boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunAsync(const std::string& session_id) = 0;

    virtual boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunByUserAsync(int64_t user_id) = 0;

    // Used by paused-session heartbeats so a long pause doesn't silently expire the run
    virtual boost::asio::awaitable<void> TouchActiveRunAsync(int64_t user_id, const std::string& session_id) = 0;

    // Telemetry ingestion

    /// Batch dedup (SET NX run:batch:{session_id}:{batch_uuid}). false = duplicate
    virtual boost::asio::awaitable<bool> ClaimBatchAsync(const std::string& session_id,
                                                         const std::string& batch_uuid) = 0;

    virtual boost::asio::awaitable<void> ReleaseClaimBatchAsync(const std::string& session_id,
                                                                const std::string& batch_uuid) = 0;

    /// Append an accepted batch to the track atomically (FCALL track_flush).
    /// nullopt = transport failure; 0 = replay (sequence_number already applied);
    /// N = number of points appended
    virtual boost::asio::awaitable<std::optional<int64_t>>
    AppendTrackBatchAsync(const std::string& session_id, std::span<const gameplay::workout_tracking::RunPoint> points,
                          double delta_meters, int64_t sequence_number) = 0;

    /// Full stored track, used by the finalizer
    virtual boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    GetTrackAsync(const std::string& session_id) = 0;

    // Idempotent close

    /// Atomic close marker (SETNX run:closing:{session_id}, short TTL).
    /// false = another coroutine (possibly on another node) is finalizing
    /// this session right now
    virtual boost::asio::awaitable<bool> TryClaimClosingAsync(const std::string& session_id) = 0;

    /// Drops the closing marker after a failed finalize attempt so a retry
    /// can start over. A successful close removes it inside DropRunDataAsync
    virtual boost::asio::awaitable<void> ReleaseClosingAsync(const std::string& session_id) = 0;

    virtual boost::asio::awaitable<bool> SaveRunSummaryAsync(const StoredRunSummary& summary) = 0;

    virtual boost::asio::awaitable<std::optional<StoredRunSummary>>
    GetRunSummaryAsync(const std::string& session_id) = 0;

    // Offline upload assembly

    virtual boost::asio::awaitable<bool> InitUploadAsync(const UploadMetaRecord& meta) = 0;

    virtual boost::asio::awaitable<std::optional<UploadMetaRecord>>
    GetUploadMetaAsync(const std::string& session_id) = 0;

    /// Stores one chunk (dedup by chunk_index), returns the count of DISTINCT held chunks
    virtual boost::asio::awaitable<int32_t>
    PutUploadChunkAsync(const std::string& session_id, int32_t chunk_index,
                        std::span<const gameplay::workout_tracking::RunPoint> points) = 0;

    virtual boost::asio::awaitable<std::vector<int32_t>> GetAcceptedChunksAsync(const std::string& session_id) = 0;

    /// Concatenates held chunks in chunk_index order
    virtual boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    AssembleUploadTrackAsync(const std::string& session_id) = 0;

    /// Removes every per-run key (track, active record, upload assembly) and the user slot
    virtual boost::asio::awaitable<void> DropRunDataAsync(const std::string& session_id, int64_t user_id) = 0;
};

} // namespace run::repository
