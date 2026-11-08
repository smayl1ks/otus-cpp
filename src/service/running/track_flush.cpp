#include "service/running/track_flush.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <map>
#include <vector>

namespace run::service::running
{

std::vector<std::pair<std::string, std::string>> EncodeTrackBatches(std::span<const TrackPoint> points)
{
    std::map<std::string, nlohmann::json> groups;
    for (const auto& point : points)
    {
        auto& rows = groups[point.session_id.empty() ? std::to_string(point.run_id) : point.session_id];
        // push_back on a null json turns it into an array
        rows.push_back({
            point.sequence_number,
            fmt::format("{}", point.latitude),
            fmt::format("{}", point.longitude),
            fmt::format("{}", point.speed),
            std::to_string(point.timestamp_ms),
            fmt::format("{:.2f}", point.delta_meters),
            point.delta_points,
        });
    }

    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(groups.size());
    for (auto& [tag, rows] : groups)
    {
        out.emplace_back(tag, rows.dump());
    }
    return out;
}

boost::asio::awaitable<std::optional<int64_t>> FlushTrackBatchAsync(run::redis::RedisSentinelClient& client,
                                                                    std::string tag, std::string points_json,
                                                                    std::chrono::seconds ttl)
{
    // Both keys carry tag as a Redis hash tag so they land on one shard; the
    // track key doubles as the routing key
    const std::string track_key = fmt::format("run:{{{}}}:track", tag);
    const std::string active_key = fmt::format("run:{{{}}}:active", tag);
    const std::vector<std::string> keys{track_key, active_key};
    const std::vector<std::string> args{points_json, std::to_string(ttl.count())};
    co_return co_await client.FcallAsync(track_key, "track_flush", keys, args);
}

} // namespace run::service::running
