#pragma once

#include "repository/i_track_repository.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

#include <memory>

namespace run::storage::redis
{

class RedisTrackRepository : public repository::ITrackRepository
{
public:
    explicit RedisTrackRepository(std::shared_ptr<ShardedRedisManager> redis_manager);
    ~RedisTrackRepository() override = default;

    boost::asio::awaitable<bool> PushTrackPointAsync(uint64_t run_id, const gameplay::workout_tracking::RunPoint& point,
                                                     double delta_meters = 0.0, int32_t delta_points = 0) override;

    boost::asio::awaitable<bool> PushTrackBatchAsync(uint64_t run_id,
                                                     std::span<const gameplay::workout_tracking::RunPoint> points,
                                                     double total_delta_meters = 0.0,
                                                     int32_t total_delta_points = 0) override;

    boost::asio::awaitable<bool> InitActiveRunAsync(const std::string& session_id, int64_t user_id, int64_t team_id,
                                                    int64_t start_timestamp) override;

    boost::asio::awaitable<std::optional<repository::ActiveRunStats>>
    GetActiveRunStatsAsync(const std::string& session_id) override;

    boost::asio::awaitable<bool> CheckActiveSessionExistsAsync(int64_t user_id) override;

    boost::asio::awaitable<bool> FinalizeActiveRunAsync(const std::string& session_id, int64_t user_id) override;

    boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    GetTrackAsync(const std::string& session_id) override;

private:
    std::shared_ptr<ShardedRedisManager> m_redis_manager;
};

} // namespace run::storage::redis
