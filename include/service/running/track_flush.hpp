#pragma once

#include "storage/redis/redis_client.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace run::service::running
{

struct TrackPoint
{
    uint64_t run_id{0};
    std::string session_id;
    double latitude{0.0};
    double longitude{0.0};
    double speed{0.0};
    int64_t timestamp_ms{0};
    double delta_meters{0.0};
    int32_t delta_points{0};
    int64_t sequence_number{0};
};

/// Groups points and encodes them into JSON arrays for track_flush.
/// Returns (tag, points_json) pairs, one per routing group
std::vector<std::pair<std::string, std::string>> EncodeTrackBatches(std::span<const TrackPoint> points);

/// Lua function library for atomic batch flushing in Redis (deduplicates by sequence number,
/// updates tracking lists/hashes, and refreshes TTLs). Loaded via extra_setup_commands
constexpr std::string_view kTrackFlushFunctionLib = R"lua(#!lua name=track_flush
local function track_flush(keys, args)
  local last = redis.call('HGET', keys[2], 'last_sequence_number')
  local last_seq = last and tonumber(last) or -1
  local points = cjson.decode(args[1])
  local fresh = {}
  local dist, pts_delta = 0, 0
  local max_seq, max_lat, max_lon, max_ts = -1, '0', '0', '0'
  for _, p in ipairs(points) do
    local seq = tonumber(p[1])
    if seq > last_seq then
      fresh[#fresh + 1] = string.format('%s,%s,%s,%s', p[2], p[3], p[4], p[5])
      dist = dist + tonumber(p[6])
      pts_delta = pts_delta + tonumber(p[7])
      if seq > max_seq then
        max_seq, max_lat, max_lon, max_ts = seq, p[2], p[3], p[5]
      end
    end
  end
  if #fresh == 0 then
    return 0
  end
  redis.call('RPUSH', keys[1], unpack(fresh))
  if dist > 0 then
    redis.call('HINCRBYFLOAT', keys[2], 'total_distance_meters', string.format('%.2f', dist))
  end
  if pts_delta ~= 0 then
    redis.call('HINCRBY', keys[2], 'total_points_count', pts_delta)
  end
  redis.call('HSET', keys[2], 'last_sequence_number', max_seq,
    'last_lat', max_lat, 'last_lon', max_lon, 'last_ts_ms', max_ts)
  redis.call('EXPIRE', keys[1], args[2])
  redis.call('EXPIRE', keys[2], args[2])
  return #fresh
end

redis.register_function('track_flush', track_flush)
)lua";

/// Atomically flushes an encoded track batch via FCALL track_flush (safe to retry).
/// Returns the number of fresh points applied, or std::nullopt on error
boost::asio::awaitable<std::optional<int64_t>> FlushTrackBatchAsync(run::redis::RedisSentinelClient& client,
                                                                    std::string tag, std::string points_json,
                                                                    std::chrono::seconds ttl);

} // namespace run::service::running
