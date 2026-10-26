#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "repository/i_active_run_store.hpp"
#include "repository/i_run_history_repository.hpp"
#include "service/running/run_finalizer.hpp"
#include "session.pb.h"

#include <boost/asio/awaitable.hpp>
#include <memory>

namespace run::service::running
{

// Offline upload flow: a run that finished entirely without connectivity is
// uploaded in three phases over the same WS.
//
// Idempotency: Init resumes by session_id (returns already-accepted chunks),
// Chunk dedups by (session_id, chunk_index), Commit replays the stored
// RunSummary. Chunks may arrive out of order; the final verdict always
// happens in RunFinalizer over the assembled track.
struct UploadLimits
{
    uint32_t max_points_per_chunk{1'000}; // keeps a WS frame well under proxy limits
    int32_t max_total_points{43'200};     // 12 h at 1 Hz (Tmax)
    int64_t max_duration_seconds{43'200};
};

class OfflineUploadService
{
public:
    OfflineUploadService(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                         std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                         std::shared_ptr<RunFinalizer> run_finalizer, UploadLimits limits = {});

    boost::asio::awaitable<run::proto::session::OfflineRunInitResponse>
    InitAsync(int64_t user_id, const run::proto::session::OfflineRunInitRequest& request);

    boost::asio::awaitable<run::proto::session::OfflineRunChunkResponse>
    ChunkAsync(int64_t user_id, const run::proto::session::OfflineRunChunkRequest& request);

    boost::asio::awaitable<run::proto::session::OfflineRunCommitResponse>
    CommitAsync(int64_t user_id, const run::proto::session::OfflineRunCommitRequest& request);

private:
    static int64_t NowMs() noexcept;

    std::shared_ptr<repository::IActiveRunStore> m_store;
    std::shared_ptr<repository::IRunHistoryRepository> m_history;
    std::shared_ptr<RunFinalizer> m_finalizer;
    gameplay::anticheat::GpsAntiCheat m_anticheat; // advisory per-chunk preview only
    UploadLimits m_limits;
};

} // namespace run::service::running
