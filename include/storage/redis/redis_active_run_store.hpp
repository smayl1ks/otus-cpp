#pragma once

#include "repository/i_active_run_store.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

#include <memory>

namespace run::storage::redis
{

class RedisActiveRunStore : public repository::IActiveRunStore
{
public:
    explicit RedisActiveRunStore(std::shared_ptr<ShardedRedisManager> redis_manager);
    ~RedisActiveRunStore() override = default;

    // ---- Lifecycle -------------------------------------------------------
    boost::asio::awaitable<bool> TryClaimUserSlotAsync(int64_t user_id, const std::string& session_id) override;
    boost::asio::awaitable<void> ReleaseUserSlotAsync(int64_t user_id, const std::string& session_id) override;
    boost::asio::awaitable<bool> InitActiveRunAsync(const repository::ActiveRunRecord& record) override;
    boost::asio::awaitable<std::optional<repository::ActiveRunRecord>> GetActiveRunAsync(const std::string& session_id) override;
    boost::asio::awaitable<std::optional<repository::ActiveRunRecord>> GetActiveRunByUserAsync(int64_t user_id) override;

    // ---- Telemetry ingestion ----------------------------------------------
    boost::asio::awaitable<bool> ClaimBatchAsync(const std::string& session_id, const std::string& batch_uuid) override;
    boost::asio::awaitable<bool> AppendTrackBatchAsync(const std::string& session_id,
                                                       std::span<const gameplay::workout_tracking::RunPoint> points,
                                                       double delta_meters, int64_t sequence_number) override;
    boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>> GetTrackAsync(const std::string& session_id) override;

    // ---- Idempotent close --------------------------------------------------
    boost::asio::awaitable<bool> SaveRunSummaryAsync(const repository::StoredRunSummary& summary) override;
    boost::asio::awaitable<std::optional<repository::StoredRunSummary>> GetRunSummaryAsync(const std::string& session_id) override;

    // ---- Offline upload assembly -------------------------------------------
    boost::asio::awaitable<bool> InitUploadAsync(const repository::UploadMetaRecord& meta) override;
    boost::asio::awaitable<std::optional<repository::UploadMetaRecord>> GetUploadMetaAsync(const std::string& session_id) override;
    boost::asio::awaitable<int32_t> PutUploadChunkAsync(const std::string& session_id, int32_t chunk_index,
                                                        std::span<const gameplay::workout_tracking::RunPoint> points) override;
    boost::asio::awaitable<std::vector<int32_t>> GetAcceptedChunksAsync(const std::string& session_id) override;
    boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>> AssembleUploadTrackAsync(const std::string& session_id) override;
    boost::asio::awaitable<void> DropRunDataAsync(const std::string& session_id, int64_t user_id) override;

private:
    std::shared_ptr<ShardedRedisManager> m_redis;
};

} // namespace run::storage::redis
