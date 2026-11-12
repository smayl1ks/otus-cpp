#pragma once

#include <string_view>

namespace run::service::running
{

/// Lua function library for atomic batch flushing in Redis, loaded via
/// extra_setup_commands (FUNCTION LOAD REPLACE) at startup and applied by
/// RedisActiveRunStore::AppendTrackBatchAsync.
///
/// Contract (batch-atomic): args = {rows_json, sequence_number, points_count,
/// delta_meters, ttl_seconds}. rows_json is an array of [lat, lon, speed, ts]
/// for every accepted point of the batch. The sequence-number replay gate is
/// checked once for the whole batch; either all points are appended or none.
/// last_point_ts_ms is the name read back by GetActiveRunAsync (seam anchor).
constexpr std::string_view kTrackFlushFunctionLib = R"lua(#!lua name=track_flush
local function track_flush(keys, args)
  local last = redis.call('HGET', keys[2], 'last_sequence_number')
  local last_seq = last and tonumber(last) or -1
  local batch_seq = tonumber(args[2])
  if batch_seq <= last_seq then
    return 0
  end
  local rows = cjson.decode(args[1])
  local fresh = {}
  for _, r in ipairs(rows) do
    fresh[#fresh + 1] = string.format('%s,%s,%s,%s', r[1], r[2], r[3], r[4])
  end
  redis.call('RPUSH', keys[1], unpack(fresh))
  redis.call('HINCRBYFLOAT', keys[2], 'total_distance_meters', args[4])
  redis.call('HINCRBY', keys[2], 'total_points_count', args[3])
  local last_row = rows[#rows]
  redis.call('HSET', keys[2], 'last_sequence_number', batch_seq,
    'last_lat', last_row[1], 'last_lon', last_row[2], 'last_point_ts_ms', last_row[4])
  redis.call('EXPIRE', keys[1], args[5])
  redis.call('EXPIRE', keys[2], args[5])
  return #fresh
end

redis.register_function('track_flush', track_flush)
)lua";

} // namespace run::service::running
