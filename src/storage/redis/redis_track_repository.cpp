
#include "storage/redis/redis_track_repository.hpp"
#include "logger/logger.hpp"

#include <fmt/format.h>

namespace run::storage::redis
{

RedisTrackRepository::RedisTrackRepository(std::shared_ptr<ShardedRedisManager> redis_manager)
    : m_redis_manager(std::move(redis_manager))
{
}

boost::asio::awaitable<bool>
RedisTrackRepository::PushTrackPointAsync(uint64_t run_id, const gameplay::workout_tracking::RunPoint& point,
                                          double delta_meters, int32_t delta_points)
{
    if (!m_redis_manager)
    {
        co_return false;
    }

    DeferredTrackEvent ev{.run_id = run_id,
                          .latitude = point.coord.latitude,
                          .longitude = point.coord.longitude,
                          .speed = point.speed_ms.value_or(0.0f),
                          .timestamp = point.RecordedAtMs(),
                          .delta_meters = delta_meters,
                          .delta_points = delta_points};

    co_return co_await m_redis_manager->PushTrackEventAsync(ev);
}

boost::asio::awaitable<bool>
RedisTrackRepository::PushTrackBatchAsync(uint64_t run_id, std::span<const gameplay::workout_tracking::RunPoint> points,
                                          double total_delta_meters, int32_t total_delta_points)
{
    if (!m_redis_manager || points.empty())
    {
        co_return (points.empty());
    }

    const double delta_per_point = total_delta_meters / static_cast<double>(points.size());
    std::vector<DeferredTrackEvent> events;
    events.reserve(points.size());

    for (std::size_t i = 0; i < points.size(); ++i)
    {
        events.push_back(DeferredTrackEvent{
            .run_id = run_id,
            .latitude = points[i].coord.latitude,
            .longitude = points[i].coord.longitude,
            .speed = points[i].speed_ms.value_or(0.0f),
            .timestamp = points[i].RecordedAtMs(),
            .delta_meters = delta_per_point,
            .delta_points = (i == points.size() - 1) ? total_delta_points : 0
        });
    }

    co_return co_await m_redis_manager->PushTrackBatchAsync(std::move(events));
}

boost::asio::awaitable<bool> RedisTrackRepository::InitActiveRunAsync(const std::string& session_id, int64_t user_id,
                                                                      int64_t team_id, int64_t start_timestamp)
{
    if (!m_redis_manager)
    {
        co_return false;
    }

    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    co_await m_redis_manager->SetAsync(user_key, session_id, std::chrono::hours(24));

    const std::string run_key = fmt::format("run:active:{}", session_id);
    const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const std::vector<std::pair<std::string, std::string>> fields = {
        {"user_id", std::to_string(user_id)},
        {"team_id", std::to_string(team_id)},
        {"start_timestamp", std::to_string(start_timestamp)},
        {"distance_meters", "0.0"},
        {"uram_points", "0"},
        {"points_count", "0"},
        {"status", "active"},
        {"last_ttl_refresh", std::to_string(now_sec)}
    };

    co_return co_await m_redis_manager->HSetMultipleAsync(run_key, fields, std::chrono::hours(24));
}

boost::asio::awaitable<std::optional<repository::ActiveRunStats>>
RedisTrackRepository::GetActiveRunStatsAsync(const std::string& session_id)
{
    if (!m_redis_manager)
    {
        co_return std::nullopt;
    }

    const std::string run_key = fmt::format("run:active:{}", session_id);
    auto fields = co_await m_redis_manager->HGetAllAsync(run_key);
    if (fields.empty())
    {
        co_return std::nullopt;
    }

    repository::ActiveRunStats stats;
    try
    {
        if (fields.count("user_id"))
            stats.user_id = std::stoll(fields["user_id"]);
        if (fields.count("team_id"))
            stats.team_id = std::stoll(fields["team_id"]);
        if (fields.count("start_timestamp"))
            stats.start_timestamp = std::stoll(fields["start_timestamp"]);
        if (fields.count("distance_meters"))
            stats.distance_meters = std::stod(fields["distance_meters"]);
        if (fields.count("points_count"))
            stats.points_count = std::stoi(fields["points_count"]);
        if (fields.count("status"))
            stats.status = fields["status"];
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("Failed parsing active run stats for {}: {}", session_id, ex.what());
    }

    co_return stats;
}

boost::asio::awaitable<bool> RedisTrackRepository::CheckActiveSessionExistsAsync(int64_t user_id)
{
    if (!m_redis_manager)
    {
        co_return false;
    }

    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    co_return co_await m_redis_manager->ExistsAsync(user_key);
}

boost::asio::awaitable<bool> RedisTrackRepository::FinalizeActiveRunAsync(const std::string& session_id,
                                                                          int64_t user_id)
{
    if (!m_redis_manager)
    {
        co_return false;
    }

    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    co_await m_redis_manager->DeleteAsync(user_key);

    const std::string run_key = fmt::format("run:active:{}", session_id);
    co_await m_redis_manager->HSetAsync(run_key, "status", "finished", std::chrono::hours(1));

    try
    {
        uint64_t run_id = std::stoull(session_id);
        m_redis_manager->InvalidateTtl(run_id);
    }
    catch (...)
    {
    }

    co_return true;
}

boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
RedisTrackRepository::GetTrackAsync(const std::string& session_id)
{
    std::vector<gameplay::workout_tracking::RunPoint> track;
    if (!m_redis_manager || session_id.empty())
    {
        co_return track;
    }

    uint64_t run_id = 0;
    try
    {
        run_id = std::stoull(session_id);
    }
    catch (const std::exception&)
    {
        LOG_WARN("GetTrackAsync: non-numeric session_id '{}'", session_id);
        co_return track;
    }

    const std::string track_key = fmt::format("run:track:{}", run_id);
    const auto raw_points = co_await m_redis_manager->LRangeAsync(track_key, 0, -1);

    track.reserve(raw_points.size());
    for (const auto& raw : raw_points)
    {
        // Format: "lat,lon,speed,ts" (speed in m/s, ts in epoch ms)
        const auto lat_end = raw.find(',');
        const auto lon_end = lat_end == std::string::npos ? std::string::npos : raw.find(',', lat_end + 1);
        const auto speed_end = lon_end == std::string::npos ? std::string::npos : raw.find(',', lon_end + 1);
        if (speed_end == std::string::npos)
        {
            continue;
        }

        try
        {
            gameplay::workout_tracking::RunPoint p;
            p.coord.latitude = std::stod(raw.substr(0, lat_end));
            p.coord.longitude = std::stod(raw.substr(lat_end + 1, lon_end - lat_end - 1));
            p.speed_ms = std::stof(raw.substr(lon_end + 1, speed_end - lon_end - 1));
            p.recorded_at = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(std::stoll(raw.substr(speed_end + 1))));
            track.push_back(std::move(p));
        }
        catch (const std::exception&)
        {
            LOG_WARN("GetTrackAsync: skipping malformed track point '{}'", raw);
        }
    }

    co_return track;
}

} // namespace run::storage::redis
