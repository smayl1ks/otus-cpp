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

namespace net = boost::asio;

/// Configuration limits for offline workout uploads
struct UploadLimits
{
    uint32_t max_points_per_chunk{1000};
    int32_t max_total_points{43200}; ///< 12 hours at 1 Hz
    int64_t max_duration_seconds{43200};
};

/// Handles offline workout uploads in three phases over WebSocket (Init, Chunk, Commit).
class OfflineUploadService
{
public:
    OfflineUploadService(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                         std::shared_ptr<repository::IRunHistoryRepository> history_repository,
                         std::shared_ptr<RunFinalizer> run_finalizer, UploadLimits limits = {});

    net::awaitable<run::proto::session::OfflineRunInitResponse>
    InitAsync(int64_t user_id, const run::proto::session::OfflineRunInitRequest& request);

    net::awaitable<run::proto::session::OfflineRunChunkResponse>
    ChunkAsync(int64_t user_id, const run::proto::session::OfflineRunChunkRequest& request);

    net::awaitable<run::proto::session::OfflineRunCommitResponse>
    CommitAsync(int64_t user_id, const run::proto::session::OfflineRunCommitRequest& request);

private:
    std::shared_ptr<repository::IActiveRunStore> m_run_store;
    std::shared_ptr<repository::IRunHistoryRepository> m_run_history;
    std::shared_ptr<RunFinalizer> m_finalizer;
    gameplay::anticheat::GpsAntiCheat m_anticheat; // advisory per-chunk preview only
    UploadLimits m_limits;
};

} // namespace run::service::running
