#include "repository/redis_hex_map_cache.hpp"

#include "logger/logger.hpp"

#include <charconv>
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

namespace run::repository
{

namespace
{

using repository::HexOwnershipRecord;

/// 30-day TTL for cached map data
constexpr auto kCacheTtl = std::chrono::hours(24 * 30);

/// Snapshot hex cap to protect against pathological payloads (legitimate captures stay well below).
constexpr std::size_t kSnapshotHexCap = 20000;

std::string ZoneKey(uint64_t zone6)
{
    /// Returns the Redis hash-tagged zone key (`map:zone:{<zone6>}:hexes`) to ensure
    /// single-shard locality for both batched writes and per-key reads
    return fmt::format("map:zone:{{{:x}}}:hexes", zone6);
}

std::string HexKey(uint64_t zone6, uint64_t hex9)
{
    return fmt::format("map:zone:{{{:x}}}:hex:{:x}", zone6, hex9);
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
    Int value{};
    const auto* first = text.data();
    if (std::from_chars(first, first + text.size(), value).ec != std::errc{} ||
        first + text.size() != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

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

boost::asio::awaitable<bool> RedisHexMapCache::WriteAsync(const HexOwnershipRecord& hex, uint64_t parent_zone6)
{
    co_return co_await WriteZoneBatchAsync(std::span<const HexOwnershipRecord>(&hex, 1), parent_zone6);
}

boost::asio::awaitable<bool> RedisHexMapCache::WriteZoneBatchAsync(std::span<const HexOwnershipRecord> hexes,
                                                                   uint64_t parent_zone6)
{
    if (m_client == nullptr || hexes.empty())
    {
        co_return false;
    }

    const std::string ttl = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kCacheTtl).count());
    const std::string zone_key = ZoneKey(parent_zone6);

    // Whole zone batch in ONE round trip: SADD all members + EXPIRE, then
    // HSET + EXPIRE per hex. All keys of one zone live on the zone's shard
    boost::redis::request commands;
    std::vector<std::string> members;
    members.reserve(hexes.size());
    for (const auto& hex : hexes)
    {
        members.push_back(ToHex(hex.h3_index));
    }
    commands.push_range("SADD", zone_key, members.begin(), members.end());
    commands.push("EXPIRE", zone_key, ttl);

    for (const auto& hex : hexes)
    {
        const std::string hex_key = HexKey(parent_zone6, hex.h3_index);
        commands.push("HSET", hex_key, "owner_user_id", std::to_string(hex.owner_user_id), "owner_username",
                      hex.owner_username, "owner_color_hex", hex.owner_color_hex, "score", std::to_string(hex.score));
        commands.push("EXPIRE", hex_key, ttl);
    }

    co_return co_await m_client->ExecBatchAsync(zone_key, commands);
}

boost::asio::awaitable<std::vector<HexOwnershipRecord>>
RedisHexMapCache::GetSnapshotAsync(std::span<const uint64_t> parent_zones6)
{
    std::vector<HexOwnershipRecord> snapshot;
    std::unordered_set<uint64_t> seen;
    if (m_client == nullptr)
    {
        co_return snapshot;
    }

    std::vector<std::string> hex_keys;
    std::vector<uint64_t> hex_ids;
    for (const uint64_t zone6 : parent_zones6)
    {
        const auto members = co_await m_client->SMembersAsync(ZoneKey(zone6));
        for (const auto& member : members)
        {
            const auto hex9 = FromHex(member);
            if (!hex9.has_value() || !seen.insert(*hex9).second)
            {
                continue;
            }
            hex_ids.push_back(*hex9);
            hex_keys.push_back(HexKey(zone6, *hex9));
        }
    }

    if (hex_keys.size() > kSnapshotHexCap)
    {
        LOG_WARN("HexMapCache snapshot truncated: {} hexes requested, cap {}", hex_keys.size(), kSnapshotHexCap);
        hex_keys.resize(kSnapshotHexCap);
        hex_ids.resize(kSnapshotHexCap);
    }

    const auto hashes = co_await m_client->HGetAllManyAsync(hex_keys);
    std::vector<uint64_t> missing_ids;
    for (std::size_t i = 0; i < hex_ids.size() && i < hashes.size(); ++i)
    {
        std::optional<HexOwnershipRecord> record;
        if (!hashes[i].empty())
        {
            record = FromHash(hex_ids[i], hashes[i]);
        }
        if (record.has_value())
        {
            snapshot.push_back(std::move(*record));
        }
        else
        {
            // Zone set lists a hex whose ownership hash expired, was never
            // written (pipeline torn mid-batch) or does not parse. The
            // fallback source repairs it below instead of dropping the hex
            missing_ids.push_back(hex_ids[i]);
        }
    }

    if (!missing_ids.empty() && m_fallback_source != nullptr)
    {
        const auto restored = co_await m_fallback_source->GetByHexIdsAsync(missing_ids);
        LOG_INFO("HexMapCache fallback restored {} of {} missing hex(es) from PostgreSQL", restored.size(),
                 missing_ids.size());

        std::unordered_map<uint64_t, std::vector<HexOwnershipRecord>> per_zone;
        for (const auto& record : restored)
        {
            const auto zone6 = m_parent_zone6_of != nullptr ? m_parent_zone6_of(record.h3_index) : std::nullopt;
            if (zone6.has_value())
            {
                per_zone[*zone6].push_back(record);
            }
            snapshot.push_back(record);
        }
        for (const auto& [zone6, hexes] : per_zone)
        {
            if (!(co_await WriteZoneBatchAsync(hexes, zone6)))
            {
                LOG_WARN("HexMapCache fallback write-back failed for {} hex(es) of zone {:x}", hexes.size(), zone6);
            }
        }
    }

    co_return snapshot;
}

} // namespace run::repository
