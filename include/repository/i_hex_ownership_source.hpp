#pragma once

#include "repository/i_hex_map_cache.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace run::repository
{

/// Read-only source of hex ownership records for the map self-heal cache path
/// (PostgreSQL durable plane abstraction)
class IHexOwnershipSource
{
public:
    virtual ~IHexOwnershipSource() = default;

    /// Loads ownership records for the given res-9 hex indexes (returns empty on failure)
    virtual boost::asio::awaitable<std::vector<HexOwnershipRecord>>
    GetByHexIdsAsync(std::span<const uint64_t> h3_indexes) = 0;
};

} // namespace run::repository
