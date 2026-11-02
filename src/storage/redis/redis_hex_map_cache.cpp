#include "storage/redis/redis_hex_map_cache.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <charconv>
#include <fmt/format.h>
#include <unordered_set>

namespace run::storage::redis
{

namespace
{
using repository::HexOwnershipRecord;

constexpr auto kCacheTtl = std::chrono::hours(24 * 30);
constexpr std::size_t kSnapshotHexCap = 5000;

std::string ZoneKey(uint64_t zone6)
{
    return fmt::format("map:zone:{:x}:hexes", zone6);
}

std::string HexKey(uint64_t hex9)
{
    return fmt::format("map:hex:{:x}", hex9);
}

std::string ToHex(uint64_t value)
{
    return fmt::format("{:x}", value);
}

std::optional<uint64_t> FromHex(const std::string& text)
{
    uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

template <typename Int>
std::optional<Int> ParseNumber(const std::string& text)
{
    Int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (ec != std::errc{} || ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

// Crash-safe: values come from Redis (possibly foreign/corrupt). Malformed
// numeric fields reject the whole record instead of throwing out of a
// detached coroutine (std::stoull/stoi used to std::terminate the node).
std::optional<HexOwnershipRecord> FromHash(uint64_t hex9, const std::unordered_map<std::string, std::string>& fields)
{
    const auto owner_id = [&]
    {
        const auto it = fields.find("owner_user_id");
        return it == fields.end() ? std::nullopt : ParseNumber<uint64_t>(it->second);
    }();
    if (!owner_id.has_value())
    {
        return std::nullopt;
    }

    HexOwnershipRecord record;
    record.h3_index = hex9;
    record.owner_user_id = *owner_id;

    if (auto it = fields.find("owner_username"); it != fields.end())
    {
        record.owner_username = it->second;
    }
    if (auto it = fields.find("owner_color_hex"); it != fields.end())
    {
        record.owner_color_hex = it->second;
    }
    if (auto it = fields.find("score"); it != fields.end())
    {
        if (const auto score = ParseNumber<int32_t>(it->second); score.has_value())
        {
            record.score = *score;
        }
    }
    return record;
}

} // namespace

RedisHexMapCache::RedisHexMapCache(std::shared_ptr<ShardedRedisManager> redis) : m_redis(std::move(redis))
{
}

boost::asio::awaitable<bool> RedisHexMapCache::WriteAsync(const HexOwnershipRecord& hex, uint64_t parent_zone6)
{
    co_return co_await WriteZoneBatchAsync(std::span<const HexOwnershipRecord>(&hex, 1), parent_zone6);
}

boost::asio::awaitable<bool> RedisHexMapCache::WriteZoneBatchAsync(std::span<const HexOwnershipRecord> hexes,
                                                                   uint64_t parent_zone6)
{
    if (!m_redis || hexes.empty())
    {
        co_return false;
    }

    const auto ttl = std::chrono::duration_cast<std::chrono::seconds>(kCacheTtl);
    const std::string ttl_str = std::to_string(ttl.count());
    const std::string zone_key = ZoneKey(parent_zone6);

    // Whole zone batch in ONE round trip: SADD all members + EXPIRE, then
    // HSET + EXPIRE per hex (16 hexes used to cost 32 round trips).
    boost::redis::request commands;
    std::vector<std::string> members;
    members.reserve(hexes.size());
    for (const auto& hex : hexes)
    {
        members.push_back(ToHex(hex.h3_index));
    }
    commands.push_range("SADD", zone_key, members.begin(), members.end());
    commands.push("EXPIRE", zone_key, ttl_str);

    for (const auto& hex : hexes)
    {
        const std::string hex_key = HexKey(hex.h3_index);
        commands.push("HSET", hex_key, "owner_user_id", std::to_string(hex.owner_user_id), "owner_username",
                      hex.owner_username, "owner_color_hex", hex.owner_color_hex, "score", std::to_string(hex.score));
        commands.push("EXPIRE", hex_key, ttl_str);
    }

    co_return co_await m_redis->ExecBatchAsync(zone_key, commands);
}

boost::asio::awaitable<std::vector<HexOwnershipRecord>>
RedisHexMapCache::GetSnapshotAsync(std::span<const uint64_t> parent_zones6)
{
    std::vector<HexOwnershipRecord> snapshot;
    std::unordered_set<uint64_t> seen;

    // Zone sets are cheap (<=128 zones), so they stay sequential; the heavy
    // part — one HGETALL per hex, up to thousands — is pipelined below.
    std::vector<std::string> hex_keys;
    std::vector<uint64_t> hex_ids;
    for (const uint64_t zone6 : parent_zones6)
    {
        const auto members = co_await m_redis->SMembersAsync(ZoneKey(zone6));
        for (const auto& member : members)
        {
            const auto hex9 = FromHex(member);
            if (!hex9.has_value() || !seen.insert(*hex9).second)
            {
                continue;
            }
            hex_ids.push_back(*hex9);
            hex_keys.push_back(HexKey(*hex9));
        }
    }

    const auto hashes = co_await m_redis->HGetAllManyAsync(hex_keys);
    for (std::size_t i = 0; i < hex_ids.size() && i < hashes.size(); ++i)
    {
        if (hashes[i].empty())
        {
            continue; // zone set points at a hex whose hash expired — skip
        }
        if (auto record = FromHash(hex_ids[i], hashes[i]); record.has_value())
        {
            snapshot.push_back(std::move(*record));
            if (snapshot.size() >= kSnapshotHexCap)
            {
                LOG_WARN("Hex map snapshot hit the cap of {} hexes, truncating", kSnapshotHexCap);
                break;
            }
        }
        else
        {
            LOG_WARN("Hex map snapshot skipped corrupt hash for hex {:x}", hex_ids[i]);
        }
    }

    co_return snapshot;
}

} // namespace run::storage::redis
