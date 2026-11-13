#pragma once

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace run::repository
{

/// Represents a captured hex for map viewport snapshots and capture events
struct HexOwnershipRecord
{
    uint64_t h3_index{0};
    uint64_t owner_user_id{0};
    std::string owner_username;
    std::string owner_color_hex;
    int32_t score{0};
};

/// Redis-backed map cache storing zone sets and per-hex ownership hashes for viewport snapshots,
/// with self-healing fallback support
class IHexMapCache
{
public:
    virtual ~IHexMapCache() = default;

    /// Upserts a single captured hex into its zone set and ownership hash
    virtual boost::asio::awaitable<bool> WriteAsync(const HexOwnershipRecord& hex, uint64_t parent_zone6) = 0;

    /// Upserts a batched list of captured hexes for a single zone in a single pipelined round-trip
    virtual boost::asio::awaitable<bool> WriteZoneBatchAsync(std::span<const HexOwnershipRecord> hexes,
                                                             uint64_t parent_zone6) = 0;

    /// Collects deduplicated captured hexes across the given res-6 zones
    virtual boost::asio::awaitable<std::vector<HexOwnershipRecord>>
    GetSnapshotAsync(std::span<const uint64_t> parent_zones6) = 0;

    virtual boost::asio::awaitable<std::optional<std::size_t>> WarmUpAsync() = 0;
};

} // namespace run::repository
