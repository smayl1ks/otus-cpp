#pragma once

#include "gameplay/workout_tracking/workout_session.hpp"

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace run::repository
{

struct ActiveRunStats
{
    int64_t user_id{0};
    int64_t team_id{0};
    int64_t start_timestamp{0};
    double distance_meters{0.0};
    int32_t points_count{0};
    int32_t uram_points{0};
    std::string status{"active"};
};

class ITrackRepository
{
public:
    virtual ~ITrackRepository() = default;

    virtual boost::asio::awaitable<bool> PushTrackPointAsync(uint64_t run_id,
                                                             const gameplay::workout_tracking::RunPoint& point,
                                                             double delta_meters = 0.0, int32_t delta_points = 0) = 0;

    virtual boost::asio::awaitable<bool>
    PushTrackBatchAsync(uint64_t run_id, std::span<const gameplay::workout_tracking::RunPoint> points,
                        double total_delta_meters = 0.0, int32_t total_delta_points = 0) = 0;

    virtual boost::asio::awaitable<bool> InitActiveRunAsync(const std::string& session_id, int64_t user_id,
                                                            int64_t team_id, int64_t start_timestamp) = 0;

    virtual boost::asio::awaitable<std::optional<ActiveRunStats>>
    GetActiveRunStatsAsync(const std::string& session_id) = 0;

    virtual boost::asio::awaitable<bool> CheckActiveSessionExistsAsync(int64_t user_id) = 0;

    virtual boost::asio::awaitable<bool> FinalizeActiveRunAsync(const std::string& session_id, int64_t user_id) = 0;
};

} // namespace run::repository
