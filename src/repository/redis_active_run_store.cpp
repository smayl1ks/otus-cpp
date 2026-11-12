#include "repository/redis_active_run_store.hpp"

#include "logger/logger.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <sstream>
#include <string_view>

namespace run::repository
{

namespace
{

using gameplay::workout_tracking::RunPoint;

constexpr auto g_RunTTLSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::hours(24));

// Keys of one run share the "{session}" hash tag: one shard per run.
std::string ActiveKey(const std::string& session_id)
{
    return fmt::format("run:{{{}}}:active", session_id);
}

std::string TrackKey(const std::string& session_id)
{
    return fmt::format("run:{{{}}}:track", session_id);
}

std::string ToString(double value)
{
    return fmt::format("{:.2f}", value);
}

template <typename Int>
std::optional<Int> ParseNumber(const std::string& text)
{
    Int value{};
    const auto* first = text.data();
    if (std::from_chars(first, first + text.size(), value).ec != std::errc{} ||
        first + text.size() != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

// Track list element format, shared with the track_flush function:
// "lat,lon,speed,ts" (speed may carry the full float text).
std::optional<RunPoint> ParseTrackPoint(std::string_view raw)
{
    const auto lat_end = raw.find(',');
    if (lat_end == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto lon_end = raw.find(',', lat_end + 1);
    if (lon_end == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto speed_end = raw.find(',', lon_end + 1);
    if (speed_end == std::string_view::npos)
    {
        return std::nullopt;
    }

    RunPoint point;
    if (const auto lat = ParseNumber<double>(std::string{raw.substr(0, lat_end)}); lat.has_value())
    {
        point.coord.latitude = *lat;
    }
    else
    {
        return std::nullopt;
    }
    if (const auto lon = ParseNumber<double>(std::string{raw.substr(lat_end + 1, lon_end - lat_end - 1)});
        lon.has_value())
    {
        point.coord.longitude = *lon;
    }
    else
    {
        return std::nullopt;
    }
    if (const auto speed = ParseNumber<float>(std::string{raw.substr(lon_end + 1, speed_end - lon_end - 1)});
        speed.has_value())
    {
        point.speed_ms = *speed;
    }
    if (const auto ts = ParseNumber<int64_t>(std::string{raw.substr(speed_end + 1)}); ts.has_value())
    {
        point.recorded_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{*ts}};
    }
    else
    {
        return std::nullopt;
    }
    return point;
}

} // namespace

// Lifecycle
boost::asio::awaitable<bool> RedisActiveRunStore::TryClaimUserSlotAsync(int64_t user_id, const std::string& session_id)
{
    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    const auto claimed = co_await m_client->SetNxAsync(user_key, session_id, g_RunTTLSeconds);

    if (claimed.has_value() && *claimed)
    {
        co_return true;
    }

    // Same session re-claiming its own slot is a retry, not a conflict
    if (claimed.has_value() && !*claimed)
    {
        const auto current = co_await m_client->GetAsync(user_key);
        if (current.has_value() && *current == session_id)
        {
            co_return true;
        }
    }

    co_return false;
}

boost::asio::awaitable<void> RedisActiveRunStore::ReleaseUserSlotAsync(int64_t user_id, const std::string& session_id)
{
    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    const auto current = co_await m_client->GetAsync(user_key);
    if (current.has_value() && *current == session_id)
    {
        co_await m_client->DeleteAsync(user_key);
    }
    co_return;
}

boost::asio::awaitable<bool> RedisActiveRunStore::InitActiveRunAsync(const ActiveRunRecord& record)
{
    const std::vector<std::pair<std::string, std::string>> fields = {
        {"session_id", record.session_id},
        {"server_run_id", std::to_string(record.server_run_id)},
        {"user_id", std::to_string(record.user_id)},
        {"team_id", std::to_string(record.team_id)},
        {"mode", std::to_string(static_cast<int>(record.mode))},
        {"visibility", std::to_string(static_cast<int>(record.visibility))},
        {"started_at_ms", std::to_string(record.started_at_ms)},
        {"total_distance_meters", ToString(record.total_distance_meters)},
        {"total_points_count", std::to_string(record.total_points_count)},
        {"last_sequence_number", std::to_string(record.last_sequence_number)},
    };

    co_await m_client->DeleteAsync(TrackKey(record.session_id));
    co_return co_await m_client->HSetMultipleAsync(ActiveKey(record.session_id), fields, g_RunTTLSeconds);
}

boost::asio::awaitable<std::optional<ActiveRunRecord>>
RedisActiveRunStore::GetActiveRunAsync(const std::string& session_id)
{
    if (session_id.empty())
    {
        co_return std::nullopt;
    }

    const auto fields = co_await m_client->HGetAllAsync(ActiveKey(session_id));
    if (fields.empty() || !fields.contains("user_id"))
    {
        co_return std::nullopt;
    }

    ActiveRunRecord record;
    record.session_id = session_id;
    const auto integer = [&fields](const char* name)
    {
        const auto it = fields.find(name);
        return it == fields.end() ? std::nullopt : ParseNumber<int64_t>(it->second);
    };
    const auto real = [&fields](const char* name)
    {
        const auto it = fields.find(name);
        return it == fields.end() ? std::nullopt : ParseNumber<double>(it->second);
    };

    try
    {
        if (const auto v = integer("server_run_id"))
        {
            record.server_run_id = *v;
        }
        if (const auto v = integer("user_id"))
        {
            record.user_id = *v;
        }
        if (const auto v = integer("team_id"))
        {
            record.team_id = *v;
        }
        if (const auto v = integer("mode"))
        {
            record.mode = static_cast<RunMode>(static_cast<int>(*v));
        }
        if (const auto v = integer("visibility"))
        {
            record.visibility = static_cast<Visibility>(static_cast<int>(*v));
        }
        if (const auto v = integer("started_at_ms"))
        {
            record.started_at_ms = *v;
        }
        if (const auto v = real("total_distance_meters"))
        {
            record.total_distance_meters = *v;
        }
        if (const auto v = integer("total_points_count"))
        {
            record.total_points_count = static_cast<int32_t>(*v);
        }
        if (const auto v = integer("last_sequence_number"))
        {
            record.last_sequence_number = *v;
        }
        if (const auto v = real("last_lat"))
        {
            record.last_lat = *v;
        }
        if (const auto v = real("last_lon"))
        {
            record.last_lon = *v;
        }
        if (const auto v = integer("last_point_ts_ms"))
        {
            record.last_point_ts_ms = *v;
        }
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("Failed parsing active run record for '{}': {}", session_id, ex.what());
        co_return std::nullopt;
    }

    co_return record;
}

boost::asio::awaitable<std::optional<ActiveRunRecord>> RedisActiveRunStore::GetActiveRunByUserAsync(int64_t user_id)
{
    const auto session_id = co_await m_client->GetAsync(fmt::format("user:active_run:{}", user_id));
    if (!session_id.has_value() || session_id->empty())
    {
        co_return std::nullopt;
    }
    co_return co_await GetActiveRunAsync(*session_id);
}

boost::asio::awaitable<void> RedisActiveRunStore::TouchActiveRunAsync(int64_t user_id, const std::string& session_id)
{
    if (session_id.empty())
    {
        co_return;
    }

    const std::string ttl = std::to_string(g_RunTTLSeconds.count());

    // Session keys share the {session_id} hash tag
    boost::redis::request session_commands;
    session_commands.push("EXPIRE", ActiveKey(session_id), ttl);
    session_commands.push("EXPIRE", TrackKey(session_id), ttl);
    if (!(co_await m_client->ExecBatchAsync(ActiveKey(session_id), session_commands)))
    {
        LOG_WARN("[active_run_store] failed to refresh TTL of session '{}'", session_id);
    }

    boost::redis::request slot_commands;
    slot_commands.push("EXPIRE", fmt::format("user:active_run:{}", user_id), ttl);
    if (!(co_await m_client->ExecBatchAsync(fmt::format("user:active_run:{}", user_id), slot_commands)))
    {
        LOG_WARN("[active_run_store] failed to refresh TTL of the run slot for user {}", user_id);
    }
}

// Telemetry ingestion
boost::asio::awaitable<bool> RedisActiveRunStore::ClaimBatchAsync(const std::string& session_id,
                                                                  const std::string& batch_uuid)
{
    if (session_id.empty() || batch_uuid.empty())
    {
        co_return false;
    }

    const auto claimed = co_await m_client->SetNxAsync(fmt::format("run:{{{}}}:batch:{}", session_id, batch_uuid), "1",
                                                       std::chrono::hours(1));
    co_return claimed.has_value() && *claimed;
}

boost::asio::awaitable<void> RedisActiveRunStore::ReleaseClaimBatchAsync(const std::string& session_id,
                                                                         const std::string& batch_uuid)
{
    if (!session_id.empty() && !batch_uuid.empty())
    {
        co_await m_client->DeleteAsync(fmt::format("run:{{{}}}:batch:{}", session_id, batch_uuid));
    }
    co_return;
}

boost::asio::awaitable<std::optional<int64_t>>
RedisActiveRunStore::AppendTrackBatchAsync(const std::string& session_id,
                                           std::span<const gameplay::workout_tracking::RunPoint> points,
                                           double delta_meters, int64_t sequence_number)
{
    if (session_id.empty() || points.empty())
    {
        co_return 0;
    }

    // Batch-atomic rows: the replay gate and accumulators are keyed by
    // batch-level scalars inside track_flush
    auto rows = nlohmann::json::array();
    for (const auto& point : points)
    {
        rows.push_back({fmt::format("{}", point.coord.latitude), fmt::format("{}", point.coord.longitude),
                        fmt::format("{}", point.speed_ms.value_or(0.0f)), std::to_string(point.RecordedAtMs())});
    }

    const std::string track_key = TrackKey(session_id);
    const std::vector<std::string> keys{track_key, ActiveKey(session_id)};
    const std::vector<std::string> args{rows.dump(), std::to_string(sequence_number), std::to_string(points.size()),
                                        fmt::format("{:.2f}", delta_meters), std::to_string(g_RunTTLSeconds.count())};
    co_return co_await m_client->FcallAsync(track_key, "track_flush", keys, args);
}

boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
RedisActiveRunStore::GetTrackAsync(const std::string& session_id)
{
    std::vector<RunPoint> track;
    if (session_id.empty())
    {
        co_return track;
    }

    const auto raw_points = co_await m_client->LRangeAsync(TrackKey(session_id), 0, -1);
    track.reserve(raw_points.size());
    for (const auto& raw : raw_points)
    {
        if (auto point = ParseTrackPoint(raw); point.has_value())
        {
            point->point_order = static_cast<uint32_t>(track.size());
            track.push_back(std::move(*point));
        }
        else
        {
            LOG_WARN("GetTrackAsync: skipping malformed track point for '{}'", session_id);
        }
    }

    co_return track;
}

// Idempotent close
boost::asio::awaitable<bool> RedisActiveRunStore::TryClaimClosingAsync(const std::string& session_id)
{
    const auto claimed = co_await m_client->SetNxAsync(fmt::format("run:{{{}}}:closing", session_id), "closing",
                                                       std::chrono::seconds(300));
    co_return claimed.has_value() && *claimed;
}

boost::asio::awaitable<void> RedisActiveRunStore::ReleaseClosingAsync(const std::string& session_id)
{
    if (!session_id.empty())
    {
        co_await m_client->DeleteAsync(fmt::format("run:{{{}}}:closing", session_id));
    }
    co_return;
}

boost::asio::awaitable<bool> RedisActiveRunStore::SaveRunSummaryAsync(const StoredRunSummary& summary)
{
    if (summary.session_id.empty())
    {
        co_return false;
    }

    std::string hexes;
    for (std::size_t i = 0; i < summary.claimed_hexes.size(); ++i)
    {
        if (i > 0)
        {
            hexes.push_back(',');
        }
        hexes += std::to_string(summary.claimed_hexes[i]);
    }

    const std::vector<std::pair<std::string, std::string>> fields = {
        {"session_id", summary.session_id},
        {"server_run_id", std::to_string(summary.server_run_id)},
        {"user_id", std::to_string(summary.user_id)},
        {"close_reason", std::to_string(static_cast<int>(summary.close_reason))},
        {"total_distance_meters", ToString(summary.total_distance_meters)},
        {"duration_seconds", std::to_string(summary.duration_seconds)},
        {"moving_duration_seconds", std::to_string(summary.moving_duration_seconds)},
        {"avg_pace_sec_per_km", std::to_string(summary.avg_pace_sec_per_km)},
        {"avg_speed_kmh", fmt::format("{:.2f}", summary.avg_speed_kmh)},
        {"total_points_count", std::to_string(summary.total_points_count)},
        {"hexagons_captured_count", std::to_string(summary.hexagons_captured_count)},
        {"total_score", std::to_string(summary.total_score)},
        {"claimed_hexes", hexes},
        {"verdict", std::to_string(static_cast<int>(summary.verdict))},
        {"is_valid", summary.is_valid ? "1" : "0"},
        {"started_at_ms", std::to_string(summary.started_at_ms)},
        {"finished_at_ms", std::to_string(summary.finished_at_ms)},
    };

    co_return co_await m_client->HSetMultipleAsync(fmt::format("run:{{{}}}:summary", summary.session_id), fields,
                                                   g_RunTTLSeconds);
}

boost::asio::awaitable<std::optional<StoredRunSummary>>
RedisActiveRunStore::GetRunSummaryAsync(const std::string& session_id)
{
    if (session_id.empty())
    {
        co_return std::nullopt;
    }

    const auto fields = co_await m_client->HGetAllAsync(fmt::format("run:{{{}}}:summary", session_id));
    if (fields.empty() || !fields.contains("user_id"))
    {
        co_return std::nullopt;
    }

    StoredRunSummary summary;
    summary.session_id = session_id;
    try
    {
        if (const auto it = fields.find("server_run_id"); it != fields.end())
        {
            summary.server_run_id = std::stoll(it->second);
        }
        if (const auto it = fields.find("user_id"); it != fields.end())
        {
            summary.user_id = std::stoll(it->second);
        }
        if (const auto it = fields.find("close_reason"); it != fields.end())
        {
            summary.close_reason = static_cast<RunCloseReason>(std::stoi(it->second));
        }
        if (const auto it = fields.find("total_distance_meters"); it != fields.end())
        {
            summary.total_distance_meters = std::stod(it->second);
        }
        if (const auto it = fields.find("duration_seconds"); it != fields.end())
        {
            summary.duration_seconds = static_cast<uint32_t>(std::stoul(it->second));
        }
        if (const auto it = fields.find("moving_duration_seconds"); it != fields.end())
        {
            summary.moving_duration_seconds = static_cast<uint32_t>(std::stoul(it->second));
        }
        if (const auto it = fields.find("avg_pace_sec_per_km"); it != fields.end())
        {
            summary.avg_pace_sec_per_km = static_cast<uint32_t>(std::stoul(it->second));
        }
        if (const auto it = fields.find("avg_speed_kmh"); it != fields.end())
        {
            summary.avg_speed_kmh = std::stof(it->second);
        }
        if (const auto it = fields.find("total_points_count"); it != fields.end())
        {
            summary.total_points_count = std::stoi(it->second);
        }
        if (const auto it = fields.find("hexagons_captured_count"); it != fields.end())
        {
            summary.hexagons_captured_count = static_cast<uint32_t>(std::stoul(it->second));
        }
        if (const auto it = fields.find("total_score"); it != fields.end())
        {
            summary.total_score = static_cast<uint32_t>(std::stoul(it->second));
        }
        if (const auto it = fields.find("verdict"); it != fields.end())
        {
            summary.verdict = static_cast<ValidationVerdict>(std::stoi(it->second));
        }
        if (const auto it = fields.find("is_valid"); it != fields.end())
        {
            summary.is_valid = it->second == "1";
        }
        if (const auto it = fields.find("started_at_ms"); it != fields.end())
        {
            summary.started_at_ms = std::stoll(it->second);
        }
        if (const auto it = fields.find("finished_at_ms"); it != fields.end())
        {
            summary.finished_at_ms = std::stoll(it->second);
        }
        if (const auto it = fields.find("claimed_hexes"); it != fields.end() && !it->second.empty())
        {
            std::stringstream stream{it->second};
            std::string item;
            while (std::getline(stream, item, ','))
            {
                if (!item.empty())
                {
                    summary.claimed_hexes.push_back(std::stoull(item));
                }
            }
        }
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("Failed parsing stored run summary for '{}': {}", session_id, ex.what());
        co_return std::nullopt;
    }

    co_return summary;
}

// Offline upload assembly
boost::asio::awaitable<bool> RedisActiveRunStore::InitUploadAsync(const UploadMetaRecord& meta)
{
    if (meta.session_id.empty())
    {
        co_return false;
    }

    const std::vector<std::pair<std::string, std::string>> fields = {
        {"session_id", meta.session_id},
        {"server_run_id", std::to_string(meta.server_run_id)},
        {"user_id", std::to_string(meta.user_id)},
        {"team_id", std::to_string(meta.team_id)},
        {"visibility", std::to_string(static_cast<int>(meta.visibility))},
        {"start_timestamp_ms", std::to_string(meta.start_timestamp_ms)},
        {"finish_timestamp_ms", std::to_string(meta.finish_timestamp_ms)},
        {"event_id", std::to_string(meta.event_id)},
        {"device_id", meta.device_id},
        {"app_version", meta.app_version},
        {"total_chunks", std::to_string(meta.total_chunks)},
    };

    co_await m_client->DeleteAsync(fmt::format("run:{{{}}}:upload:chunks", meta.session_id));
    co_return co_await m_client->HSetMultipleAsync(fmt::format("run:{{{}}}:upload:meta", meta.session_id), fields,
                                                   g_RunTTLSeconds);
}

boost::asio::awaitable<std::optional<UploadMetaRecord>>
RedisActiveRunStore::GetUploadMetaAsync(const std::string& session_id)
{
    if (session_id.empty())
    {
        co_return std::nullopt;
    }

    const auto fields = co_await m_client->HGetAllAsync(fmt::format("run:{{{}}}:upload:meta", session_id));
    if (fields.empty() || !fields.contains("user_id"))
    {
        co_return std::nullopt;
    }

    UploadMetaRecord meta;
    meta.session_id = session_id;
    try
    {
        if (const auto it = fields.find("server_run_id"); it != fields.end())
        {
            meta.server_run_id = std::stoll(it->second);
        }
        if (const auto it = fields.find("user_id"); it != fields.end())
        {
            meta.user_id = std::stoll(it->second);
        }
        if (const auto it = fields.find("team_id"); it != fields.end())
        {
            meta.team_id = std::stoll(it->second);
        }
        if (const auto it = fields.find("visibility"); it != fields.end())
        {
            meta.visibility = static_cast<Visibility>(std::stoi(it->second));
        }
        if (const auto it = fields.find("start_timestamp_ms"); it != fields.end())
        {
            meta.start_timestamp_ms = std::stoll(it->second);
        }
        if (const auto it = fields.find("finish_timestamp_ms"); it != fields.end())
        {
            meta.finish_timestamp_ms = std::stoll(it->second);
        }
        if (const auto it = fields.find("event_id"); it != fields.end())
        {
            meta.event_id = std::stoll(it->second);
        }
        if (const auto it = fields.find("device_id"); it != fields.end())
        {
            meta.device_id = it->second;
        }
        if (const auto it = fields.find("app_version"); it != fields.end())
        {
            meta.app_version = it->second;
        }
        if (const auto it = fields.find("total_chunks"); it != fields.end())
        {
            meta.total_chunks = std::stoi(it->second);
        }
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("Failed parsing upload meta for '{}': {}", session_id, ex.what());
        co_return std::nullopt;
    }

    co_return meta;
}

boost::asio::awaitable<int32_t>
RedisActiveRunStore::PutUploadChunkAsync(const std::string& session_id, int32_t chunk_index,
                                         std::span<const gameplay::workout_tracking::RunPoint> points)
{
    if (session_id.empty())
    {
        co_return 0;
    }

    const std::string chunk_key = fmt::format("run:{{{}}}:upload:chunk:{}", session_id, chunk_index);
    const std::string chunks_set_key = fmt::format("run:{{{}}}:upload:chunks", session_id);
    const std::string ttl = std::to_string(g_RunTTLSeconds.count());

    std::vector<std::string> raw_points;
    raw_points.reserve(points.size());
    for (const auto& point : points)
    {
        raw_points.push_back(fmt::format("{},{},{},{}", point.coord.latitude, point.coord.longitude,
                                         point.speed_ms.value_or(0.0f), point.RecordedAtMs()));
    }

    boost::redis::request commands;
    if (!raw_points.empty())
    {
        // DEL before RPUSH
        commands.push("DEL", chunk_key);
        commands.push_range("RPUSH", chunk_key, raw_points.begin(), raw_points.end());
        commands.push("EXPIRE", chunk_key, ttl);
    }

    commands.push("SADD", chunks_set_key, std::to_string(chunk_index));
    commands.push("EXPIRE", chunks_set_key, ttl);

    if (!(co_await m_client->ExecBatchAsync(chunks_set_key, commands)))
    {
        co_return 0;
    }

    const auto count = co_await m_client->SCardAsync(chunks_set_key);
    co_return static_cast<int32_t>(count.value_or(0));
}

boost::asio::awaitable<std::vector<int32_t>> RedisActiveRunStore::GetAcceptedChunksAsync(const std::string& session_id)
{
    std::vector<int32_t> accepted;
    if (session_id.empty())
    {
        co_return accepted;
    }

    const auto members = co_await m_client->SMembersAsync(fmt::format("run:{{{}}}:upload:chunks", session_id));
    accepted.reserve(members.size());
    for (const auto& item : members)
    {
        if (const auto value = ParseNumber<int32_t>(item); value.has_value())
        {
            accepted.push_back(*value);
        }
    }

    std::sort(accepted.begin(), accepted.end());
    co_return accepted;
}

boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
RedisActiveRunStore::AssembleUploadTrackAsync(const std::string& session_id)
{
    std::vector<RunPoint> track;
    if (session_id.empty())
    {
        co_return track;
    }

    const auto chunks = co_await GetAcceptedChunksAsync(session_id);
    for (const int32_t chunk_index : chunks)
    {
        const auto raw_points =
            co_await m_client->LRangeAsync(fmt::format("run:{{{}}}:upload:chunk:{}", session_id, chunk_index), 0, -1);
        for (const auto& raw : raw_points)
        {
            if (auto point = ParseTrackPoint(raw); point.has_value())
            {
                track.push_back(std::move(*point));
            }
            else
            {
                LOG_WARN("AssembleUploadTrackAsync: skipping malformed point in chunk {} of '{}'", chunk_index,
                         session_id);
            }
        }
    }

    co_return track;
}

boost::asio::awaitable<void> RedisActiveRunStore::DropRunDataAsync(const std::string& session_id, int64_t user_id)
{
    if (session_id.empty())
    {
        co_return;
    }

    const auto chunks = co_await GetAcceptedChunksAsync(session_id);
    std::vector<std::string> keys;
    keys.reserve(chunks.size() + 5);
    for (const int32_t chunk_index : chunks)
    {
        keys.push_back(fmt::format("run:{{{}}}:upload:chunk:{}", session_id, chunk_index));
    }
    keys.push_back(fmt::format("run:{{{}}}:upload:chunks", session_id));
    keys.push_back(fmt::format("run:{{{}}}:upload:meta", session_id));
    keys.push_back(TrackKey(session_id));
    keys.push_back(ActiveKey(session_id));
    keys.push_back(fmt::format("run:{{{}}}:closing", session_id));

    boost::redis::request commands;
    commands.push_range("DEL", keys.begin(), keys.end());
    if (!(co_await m_client->ExecBatchAsync(ActiveKey(session_id), commands)))
    {
        LOG_WARN("DropRunDataAsync: failed to drop run data for '{}'", session_id);
    }

    co_await ReleaseUserSlotAsync(user_id, session_id);
}

} // namespace run::repository
