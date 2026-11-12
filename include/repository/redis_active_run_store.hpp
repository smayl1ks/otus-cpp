#pragma once

#include "gameplay/workout_tracking/workout_session.hpp"
#include "repository/i_active_run_store.hpp"
#include "storage/redis/redis_client.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace run::repository
{

/// Redis-backed implementation of IActiveRunStore using Redis hash tags (run:{session}:*)
/// to guarantee single-shard locality, with sequence-guarded track flushes
class RedisActiveRunStore final : public IActiveRunStore
{
public:
    explicit RedisActiveRunStore(std::shared_ptr<run::redis::RedisSentinelClient> client) : m_client(std::move(client))
    {
    }

    // Lifecycle
    boost::asio::awaitable<bool> TryClaimUserSlotAsync(int64_t user_id, const std::string& session_id) override;
    boost::asio::awaitable<void> ReleaseUserSlotAsync(int64_t user_id, const std::string& session_id) override;
    boost::asio::awaitable<bool> InitActiveRunAsync(const ActiveRunRecord& record) override;
    boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunAsync(const std::string& session_id) override;
    boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunByUserAsync(int64_t user_id) override;
    boost::asio::awaitable<void> TouchActiveRunAsync(int64_t user_id, const std::string& session_id) override;

    // Telemetry ingestion
    boost::asio::awaitable<bool> ClaimBatchAsync(const std::string& session_id, const std::string& batch_uuid) override;
    boost::asio::awaitable<void> ReleaseClaimBatchAsync(const std::string& session_id,
                                                        const std::string& batch_uuid) override;
    boost::asio::awaitable<std::optional<int64_t>>
    AppendTrackBatchAsync(const std::string& session_id,
                          std::span<const gameplay::workout_tracking::RunPoint> points,
                          double delta_meters, int64_t sequence_number) override;
    boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    GetTrackAsync(const std::string& session_id) override;

    // Idempotent close
    boost::asio::awaitable<bool> TryClaimClosingAsync(const std::string& session_id) override;
    boost::asio::awaitable<void> ReleaseClosingAsync(const std::string& session_id) override;
    boost::asio::awaitable<bool> SaveRunSummaryAsync(const StoredRunSummary& summary) override;
    boost::asio::awaitable<std::optional<StoredRunSummary>> GetRunSummaryAsync(const std::string& session_id) override;

    // Offline upload assembly
    boost::asio::awaitable<bool> InitUploadAsync(const UploadMetaRecord& meta) override;
    boost::asio::awaitable<std::optional<UploadMetaRecord>> GetUploadMetaAsync(const std::string& session_id) override;
    boost::asio::awaitable<int32_t>
    PutUploadChunkAsync(const std::string& session_id, int32_t chunk_index,
                        std::span<const gameplay::workout_tracking::RunPoint> points) override;
    boost::asio::awaitable<std::vector<int32_t>> GetAcceptedChunksAsync(const std::string& session_id) override;
    boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    AssembleUploadTrackAsync(const std::string& session_id) override;
    boost::asio::awaitable<void> DropRunDataAsync(const std::string& session_id, int64_t user_id) override;

private:
    std::shared_ptr<run::redis::RedisSentinelClient> m_client;
};

} // namespace run::repository
