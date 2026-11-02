#pragma once

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace run::repository
{

// One captured hex as served to the map: viewport snapshot rows and
// HexagonCaptureEvent payloads are built from this shape.
struct HexOwnershipRecord
{
    uint64_t h3_index{0};
    uint64_t owner_user_id{0};
    std::string owner_username;
    std::string owner_color_hex;
    int32_t score{0};
};

// Redis-backed map cache for the viewport feature. Layout: per res-6 zone a
// SET of captured res-9 hexes (map:zone:<h6>:hexes) plus a per-hex ownership
// HASH (map:hex:<h9>). Written at run finalization right after the PostgreSQL
// commit; read by SubscribeViewport(need_snapshot=true) — the map path never
// touches PostgreSQL. Lives on the persistent master Redis (AOF), not on the
// ephemeral pub/sub fleet.
class IHexMapCache
{
public:
    virtual ~IHexMapCache() = default;

    /// Upserts one captured hex into its zone set + ownership hash.
    virtual boost::asio::awaitable<bool> WriteAsync(const HexOwnershipRecord& hex, uint64_t parent_zone6) = 0;

    /// Upserts a batch of captured hexes of ONE zone. Implementations should
    /// pipeline the whole batch into a single round trip. All keys live on
    /// the zone's shard (single-shard fleet: one request).
    virtual boost::asio::awaitable<bool>
    WriteZoneBatchAsync(std::span<const HexOwnershipRecord> hexes, uint64_t parent_zone6) = 0;

    /// Collects captured hexes across the given res-6 zones (deduplicated).
    virtual boost::asio::awaitable<std::vector<HexOwnershipRecord>>
    GetSnapshotAsync(std::span<const uint64_t> parent_zones6) = 0;
};

} // namespace run::repository
