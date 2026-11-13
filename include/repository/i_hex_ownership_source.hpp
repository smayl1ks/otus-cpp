#pragma once

#include "repository/i_hex_map_cache.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace run::repository
{

/// One entry of a hex leaderboard (up to a few best runners of the zone)
struct HexTopRunnerRecord
{
    uint64_t user_id{0};
    std::string username;
    std::string player_color_hex;
    int32_t game_points{0};
};

/// Hexagon details for the REST GET /api/v1/hexagons/{h3_index}/details endpoint;
/// has_value() == false means the hex was never captured
struct HexDetailsRecord
{
    uint64_t h3_index{0};
    std::optional<uint64_t> owner_user_id;
    std::string owner_username;
    std::string owner_color_hex;
    int32_t top_score{0};
    std::vector<HexTopRunnerRecord> top_runners;
};

/// Read-only source of hex ownership records for the map self-heal cache path
/// (PostgreSQL durable plane abstraction)
class IHexOwnershipSource
{
public:
    virtual ~IHexOwnershipSource() = default;

    /// Loads ownership records for the given res-9 hex indexes (returns empty on failure)
    virtual boost::asio::awaitable<std::vector<HexOwnershipRecord>>
    GetByHexIdsAsync(std::span<const uint64_t> h3_indexes) = 0;

    /// Loads owner info and the top runners of a single res-9 hex (nullopt when unknown hex or on failure)
    virtual boost::asio::awaitable<std::optional<HexDetailsRecord>> GetHexDetailsAsync(uint64_t h3_index) = 0;

    /// Loads every captured hex (startup map-cache warm-up; empty on failure)
    virtual boost::asio::awaitable<std::vector<HexOwnershipRecord>> GetAllCapturedAsync() = 0;
};

} // namespace run::repository
