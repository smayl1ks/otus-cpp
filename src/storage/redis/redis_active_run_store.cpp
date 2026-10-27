#include "storage/redis/redis_active_run_store.hpp"
#include "logger/logger.hpp"

#include <fmt/format.h>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace run::storage::redis
{

RedisActiveRunStore::RedisActiveRunStore(std::shared_ptr<ShardedRedisManager> redis_manager)
    : m_redis(std::move(redis_manager))
{
}

boost::asio::awaitable<bool>
RedisActiveRunStore::TryClaimUserSlotAsync(int64_t user_id, const std::string& session_id)
{
    if (!m_redis)
    {
        co_return false;
    }

    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    auto claimed = co_await m_redis->SetNxAsync(user_key, session_id, std::chrono::hours(24));

    if (claimed.has_value() && *claimed)
    {
        co_return true;
    }

    if (claimed.has_value() && !*claimed)
    {
        auto current = co_await m_redis->GetAsync(user_key);
        if (current.has_value() && *current == session_id)
        {
            co_return true;
        }
    }

    co_return false;
}

boost::asio::awaitable<void>
RedisActiveRunStore::ReleaseUserSlotAsync(int64_t user_id, const std::string& session_id)
{
    if (!m_redis)
    {
        co_return;
    }

    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    auto current = co_await m_redis->GetAsync(user_key);
    if (current.has_value() && *current == session_id)
    {
        co_await m_redis->DeleteAsync(user_key);
    }
}

boost::asio::awaitable<bool>
RedisActiveRunStore::InitActiveRunAsync(const repository::ActiveRunRecord& record)
{
    if (!m_redis)
    {
        co_return false;
    }

    const std::string run_key = fmt::format("run:active:{}", record.session_id);

    const std::vector<std::pair<std::string, std::string>> fields = {
        {"session_id", record.session_id},
        {"server_run_id", std::to_string(record.server_run_id)},
        {"user_id", std::to_string(record.user_id)},
        {"team_id", std::to_string(record.team_id)},
        {"mode", std::to_string(static_cast<int>(record.mode))},
        {"visibility", std::to_string(static_cast<int>(record.visibility))},
        {"started_at_ms", std::to_string(record.started_at_ms)},
        {"total_distance_meters", fmt::format("{:.2f}", record.total_distance_meters)},
        {"distance_meters", fmt::format("{:.2f}", record.total_distance_meters)},
        {"total_points_count", std::to_string(record.total_points_count)},
        {"points_count", std::to_string(record.total_points_count)},
        {"last_sequence_number", std::to_string(record.last_sequence_number)}
    };

    co_await m_redis->DeleteAsync(fmt::format("run:track:{}", record.session_id));
    co_return co_await m_redis->HSetMultipleAsync(run_key, fields, std::chrono::hours(24));
}

boost::asio::awaitable<std::optional<repository::ActiveRunRecord>>
RedisActiveRunStore::GetActiveRunAsync(const std::string& session_id)
{
    if (!m_redis || session_id.empty())
    {
        co_return std::nullopt;
    }

    const std::string run_key = fmt::format("run:active:{}", session_id);
    auto fields = co_await m_redis->HGetAllAsync(run_key);

    if (fields.empty() || !fields.contains("user_id"))
    {
        co_return std::nullopt;
    }

    repository::ActiveRunRecord rec;
    rec.session_id = session_id;
    try
    {
        if (fields.contains("server_run_id"))
            rec.server_run_id = std::stoll(fields.at("server_run_id"));
        if (fields.contains("user_id"))
            rec.user_id = std::stoll(fields.at("user_id"));
        if (fields.contains("team_id"))
            rec.team_id = std::stoll(fields.at("team_id"));
        if (fields.contains("mode"))
            rec.mode = static_cast<repository::RunMode>(std::stoi(fields.at("mode")));
        if (fields.contains("visibility"))
            rec.visibility = static_cast<repository::Visibility>(std::stoi(fields.at("visibility")));
        if (fields.contains("started_at_ms"))
            rec.started_at_ms = std::stoll(fields.at("started_at_ms"));
        if (fields.contains("total_distance_meters"))
            rec.total_distance_meters = std::stod(fields.at("total_distance_meters"));
        else if (fields.contains("distance_meters"))
            rec.total_distance_meters = std::stod(fields.at("distance_meters"));
        if (fields.contains("total_points_count"))
            rec.total_points_count = std::stoi(fields.at("total_points_count"));
        else if (fields.contains("points_count"))
            rec.total_points_count = std::stoi(fields.at("points_count"));
        if (fields.contains("last_sequence_number"))
            rec.last_sequence_number = std::stoll(fields.at("last_sequence_number"));
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("Failed parsing active run record for '{}': {}", session_id, ex.what());
        co_return std::nullopt;
    }

    co_return rec;
}

boost::asio::awaitable<std::optional<repository::ActiveRunRecord>>
RedisActiveRunStore::GetActiveRunByUserAsync(int64_t user_id)
{
    if (!m_redis)
    {
        co_return std::nullopt;
    }

    const std::string user_key = fmt::format("user:active_run:{}", user_id);
    auto sid = co_await m_redis->GetAsync(user_key);

    if (!sid.has_value() || sid->empty())
    {
        co_return std::nullopt;
    }

    co_return co_await GetActiveRunAsync(*sid);
}

boost::asio::awaitable<bool>
RedisActiveRunStore::ClaimBatchAsync(const std::string& session_id, const std::string& batch_uuid)
{
    if (!m_redis || session_id.empty() || batch_uuid.empty())
    {
        co_return false;
    }

    const std::string dedup_key = fmt::format("run:batch:{}:{}", session_id, batch_uuid);
    auto res = co_await m_redis->SetNxAsync(dedup_key, "1", std::chrono::hours(24));
    co_return res.has_value() && *res;
}

boost::asio::awaitable<bool>
RedisActiveRunStore::AppendTrackBatchAsync(const std::string& session_id,
                                          std::span<const gameplay::workout_tracking::RunPoint> points,
                                          double delta_meters, int64_t sequence_number)
{
    if (!m_redis || session_id.empty())
    {
        co_return false;
    }

    if (points.empty())
    {
        co_return true;
    }

    const double delta_per_point = delta_meters / static_cast<double>(points.size());
    std::vector<DeferredTrackEvent> events;
    events.reserve(points.size());

    for (std::size_t i = 0; i < points.size(); ++i)
    {
        DeferredTrackEvent ev{
            .latitude = points[i].coord.latitude,
            .longitude = points[i].coord.longitude,
            .speed = points[i].speed_ms.value_or(0.0f),
            .timestamp = points[i].RecordedAtMs(),
            .delta_meters = delta_per_point,
            .delta_points = (i == points.size() - 1) ? static_cast<int32_t>(points.size()) : 0,
            .sequence_number = (i == points.size() - 1) ? sequence_number : 0
        };
        std::strncpy(ev.session_id, session_id.c_str(), sizeof(ev.session_id) - 1);
        ev.session_id[sizeof(ev.session_id) - 1] = '\0';
        events.push_back(std::move(ev));
    }

    co_return co_await m_redis->PushTrackBatchAsync(std::move(events));
}

boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
RedisActiveRunStore::GetTrackAsync(const std::string& session_id)
{
    std::vector<gameplay::workout_tracking::RunPoint> track;
    if (!m_redis || session_id.empty())
    {
        co_return track;
    }

    const std::string track_key = fmt::format("run:track:{}", session_id);
    const auto raw_points = co_await m_redis->LRangeAsync(track_key, 0, -1);

    track.reserve(raw_points.size());
    for (const auto& raw : raw_points)
    {
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
            p.point_order = static_cast<uint32_t>(track.size());
            track.push_back(std::move(p));
        }
        catch (const std::exception& ex)
        {
            LOG_WARN("GetTrackAsync: skipping malformed point '{}' ({})", raw, ex.what());
        }
    }

    co_return track;
}

boost::asio::awaitable<bool>
RedisActiveRunStore::SaveRunSummaryAsync(const repository::StoredRunSummary& summary)
{
    if (!m_redis || summary.session_id.empty())
    {
        co_return false;
    }

    const std::string summary_key = fmt::format("run:summary:{}", summary.session_id);

    std::string hexes_str;
    for (std::size_t i = 0; i < summary.claimed_hexes.size(); ++i)
    {
        if (i > 0)
        {
            hexes_str.push_back(',');
        }
        hexes_str += std::to_string(summary.claimed_hexes[i]);
    }

    const std::vector<std::pair<std::string, std::string>> fields = {
        {"session_id", summary.session_id},
        {"server_run_id", std::to_string(summary.server_run_id)},
        {"user_id", std::to_string(summary.user_id)},
        {"close_reason", std::to_string(static_cast<int>(summary.close_reason))},
        {"total_distance_meters", fmt::format("{:.2f}", summary.total_distance_meters)},
        {"duration_seconds", std::to_string(summary.duration_seconds)},
        {"moving_duration_seconds", std::to_string(summary.moving_duration_seconds)},
        {"avg_pace_sec_per_km", std::to_string(summary.avg_pace_sec_per_km)},
        {"avg_speed_kmh", fmt::format("{:.2f}", summary.avg_speed_kmh)},
        {"total_points_count", std::to_string(summary.total_points_count)},
        {"hexagons_captured_count", std::to_string(summary.hexagons_captured_count)},
        {"total_score", std::to_string(summary.total_score)},
        {"claimed_hexes", hexes_str},
        {"verdict", std::to_string(static_cast<int>(summary.verdict))},
        {"is_valid", summary.is_valid ? "1" : "0"},
        {"started_at_ms", std::to_string(summary.started_at_ms)},
        {"finished_at_ms", std::to_string(summary.finished_at_ms)}
    };

    co_return co_await m_redis->HSetMultipleAsync(summary_key, fields, std::chrono::hours(24));
}

boost::asio::awaitable<std::optional<repository::StoredRunSummary>>
RedisActiveRunStore::GetRunSummaryAsync(const std::string& session_id)
{
    if (!m_redis || session_id.empty())
    {
        co_return std::nullopt;
    }

    const std::string summary_key = fmt::format("run:summary:{}", session_id);
    auto fields = co_await m_redis->HGetAllAsync(summary_key);

    if (fields.empty() || !fields.contains("user_id"))
    {
        co_return std::nullopt;
    }

    repository::StoredRunSummary s;
    s.session_id = session_id;
    try
    {
        if (fields.contains("server_run_id"))
            s.server_run_id = std::stoll(fields.at("server_run_id"));
        if (fields.contains("user_id"))
            s.user_id = std::stoll(fields.at("user_id"));
        if (fields.contains("close_reason"))
            s.close_reason = static_cast<repository::RunCloseReason>(std::stoi(fields.at("close_reason")));
        if (fields.contains("total_distance_meters"))
            s.total_distance_meters = std::stod(fields.at("total_distance_meters"));
        if (fields.contains("duration_seconds"))
            s.duration_seconds = static_cast<uint32_t>(std::stoul(fields.at("duration_seconds")));
        if (fields.contains("moving_duration_seconds"))
            s.moving_duration_seconds = static_cast<uint32_t>(std::stoul(fields.at("moving_duration_seconds")));
        if (fields.contains("avg_pace_sec_per_km"))
            s.avg_pace_sec_per_km = static_cast<uint32_t>(std::stoul(fields.at("avg_pace_sec_per_km")));
        if (fields.contains("avg_speed_kmh"))
            s.avg_speed_kmh = std::stof(fields.at("avg_speed_kmh"));
        if (fields.contains("total_points_count"))
            s.total_points_count = std::stoi(fields.at("total_points_count"));
        if (fields.contains("hexagons_captured_count"))
            s.hexagons_captured_count = static_cast<uint32_t>(std::stoul(fields.at("hexagons_captured_count")));
        if (fields.contains("total_score"))
            s.total_score = static_cast<uint32_t>(std::stoul(fields.at("total_score")));
        if (fields.contains("verdict"))
            s.verdict = static_cast<repository::ValidationVerdict>(std::stoi(fields.at("verdict")));
        if (fields.contains("is_valid"))
            s.is_valid = (fields.at("is_valid") == "1");
        if (fields.contains("started_at_ms"))
            s.started_at_ms = std::stoll(fields.at("started_at_ms"));
        if (fields.contains("finished_at_ms"))
            s.finished_at_ms = std::stoll(fields.at("finished_at_ms"));

        if (fields.contains("claimed_hexes") && !fields.at("claimed_hexes").empty())
        {
            std::stringstream ss(fields.at("claimed_hexes"));
            std::string item;
            while (std::getline(ss, item, ','))
            {
                if (!item.empty())
                {
                    s.claimed_hexes.push_back(std::stoull(item));
                }
            }
        }
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("Failed parsing stored run summary for '{}': {}", session_id, ex.what());
        co_return std::nullopt;
    }

    co_return s;
}

boost::asio::awaitable<bool>
RedisActiveRunStore::InitUploadAsync(const repository::UploadMetaRecord& meta)
{
    if (!m_redis || meta.session_id.empty())
    {
        co_return false;
    }

    const std::string meta_key = fmt::format("run:upload:{}:meta", meta.session_id);
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
        {"total_chunks", std::to_string(meta.total_chunks)}
    };

    co_await m_redis->DeleteAsync(fmt::format("run:upload:{}:chunks", meta.session_id));
    co_return co_await m_redis->HSetMultipleAsync(meta_key, fields, std::chrono::hours(24));
}

boost::asio::awaitable<std::optional<repository::UploadMetaRecord>>
RedisActiveRunStore::GetUploadMetaAsync(const std::string& session_id)
{
    if (!m_redis || session_id.empty())
    {
        co_return std::nullopt;
    }

    const std::string meta_key = fmt::format("run:upload:{}:meta", session_id);
    auto fields = co_await m_redis->HGetAllAsync(meta_key);

    if (fields.empty() || !fields.contains("user_id"))
    {
        co_return std::nullopt;
    }

    repository::UploadMetaRecord meta;
    meta.session_id = session_id;
    try
    {
        if (fields.contains("server_run_id"))
            meta.server_run_id = std::stoll(fields.at("server_run_id"));
        if (fields.contains("user_id"))
            meta.user_id = std::stoll(fields.at("user_id"));
        if (fields.contains("team_id"))
            meta.team_id = std::stoll(fields.at("team_id"));
        if (fields.contains("visibility"))
            meta.visibility = static_cast<repository::Visibility>(std::stoi(fields.at("visibility")));
        if (fields.contains("start_timestamp_ms"))
            meta.start_timestamp_ms = std::stoll(fields.at("start_timestamp_ms"));
        if (fields.contains("finish_timestamp_ms"))
            meta.finish_timestamp_ms = std::stoll(fields.at("finish_timestamp_ms"));
        if (fields.contains("event_id"))
            meta.event_id = std::stoll(fields.at("event_id"));
        if (fields.contains("device_id"))
            meta.device_id = fields.at("device_id");
        if (fields.contains("app_version"))
            meta.app_version = fields.at("app_version");
        if (fields.contains("total_chunks"))
            meta.total_chunks = std::stoi(fields.at("total_chunks"));
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
    if (!m_redis || session_id.empty())
    {
        co_return 0;
    }

    const std::string chunk_key = fmt::format("run:upload:{}:chunk:{}", session_id, chunk_index);
    const std::string chunks_set_key = fmt::format("run:upload:{}:chunks", session_id);

    std::vector<std::string> raw_points;
    raw_points.reserve(points.size());
    for (const auto& pt : points)
    {
        raw_points.push_back(fmt::format("{},{},{},{}", pt.coord.latitude, pt.coord.longitude,
                                         pt.speed_ms.value_or(0.0f), pt.RecordedAtMs()));
    }

    if (!raw_points.empty())
    {
        co_await m_redis->RPushRangeAsync(chunk_key, raw_points, std::chrono::hours(24));
    }

    co_await m_redis->SAddAsync(chunks_set_key, std::to_string(chunk_index), std::chrono::hours(24));
    auto count = co_await m_redis->SCardAsync(chunks_set_key);
    co_return static_cast<int32_t>(count);
}

boost::asio::awaitable<std::vector<int32_t>>
RedisActiveRunStore::GetAcceptedChunksAsync(const std::string& session_id)
{
    std::vector<int32_t> accepted;
    if (!m_redis || session_id.empty())
    {
        co_return accepted;
    }

    const std::string chunks_set_key = fmt::format("run:upload:{}:chunks", session_id);
    auto raw_members = co_await m_redis->SMembersAsync(chunks_set_key);

    accepted.reserve(raw_members.size());
    for (const auto& item : raw_members)
    {
        try
        {
            accepted.push_back(std::stoi(item));
        }
        catch (...)
        {
        }
    }

    std::sort(accepted.begin(), accepted.end());
    co_return accepted;
}

boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
RedisActiveRunStore::AssembleUploadTrackAsync(const std::string& session_id)
{
    std::vector<gameplay::workout_tracking::RunPoint> track;
    if (!m_redis || session_id.empty())
    {
        co_return track;
    }

    auto chunks = co_await GetAcceptedChunksAsync(session_id);
    for (int32_t chunk_idx : chunks)
    {
        const std::string chunk_key = fmt::format("run:upload:{}:chunk:{}", session_id, chunk_idx);
        auto raw_points = co_await m_redis->LRangeAsync(chunk_key, 0, -1);

        for (const auto& raw : raw_points)
        {
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
            catch (...)
            {
            }
        }
    }

    co_return track;
}

boost::asio::awaitable<void>
RedisActiveRunStore::DropRunDataAsync(const std::string& session_id, int64_t user_id)
{
    if (!m_redis || session_id.empty())
    {
        co_return;
    }

    auto chunks = co_await GetAcceptedChunksAsync(session_id);
    for (int32_t chunk_idx : chunks)
    {
        co_await m_redis->DeleteAsync(fmt::format("run:upload:{}:chunk:{}", session_id, chunk_idx));
    }
    co_await m_redis->DeleteAsync(fmt::format("run:upload:{}:chunks", session_id));
    co_await m_redis->DeleteAsync(fmt::format("run:upload:{}:meta", session_id));
    co_await m_redis->DeleteAsync(fmt::format("run:track:{}", session_id));
    co_await m_redis->DeleteAsync(fmt::format("run:active:{}", session_id));
    co_await m_redis->DeleteAsync(fmt::format("run:summary:{}", session_id));
    co_await ReleaseUserSlotAsync(user_id, session_id);
}

} // namespace run::storage::redis
