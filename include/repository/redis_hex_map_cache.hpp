#pragma once

#include "repository/i_hex_map_cache.hpp"
#include "repository/i_hex_ownership_source.hpp"
#include "storage/redis/redis_client.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace run::repository
{

/// Redis-backed implementation of IHexMapCache using Redis hash tags (map:zone:{<h6:x>}:*)
/// for single-shard locality per zone, with self-healing snapshot fallback
class RedisHexMapCache final : public IHexMapCache
{
public:
    using ParentZone6Resolver = std::function<std::optional<uint64_t>(uint64_t h3_index)>;

    RedisHexMapCache(std::shared_ptr<run::redis::RedisSentinelClient> client,
                     std::shared_ptr<IHexOwnershipSource> source = nullptr,
                     ParentZone6Resolver parent_zone6_of = nullptr)
        : m_client(std::move(client)), m_fallback_source(std::move(source)),
          m_parent_zone6_of(std::move(parent_zone6_of))
    {
    }

    boost::asio::awaitable<bool> WriteAsync(const HexOwnershipRecord& hex, uint64_t parent_zone6) override;

    boost::asio::awaitable<bool> WriteZoneBatchAsync(std::span<const HexOwnershipRecord> hexes,
                                                     uint64_t parent_zone6) override;

    boost::asio::awaitable<std::vector<HexOwnershipRecord>>
    GetSnapshotAsync(std::span<const uint64_t> parent_zones6) override;

    boost::asio::awaitable<std::optional<std::size_t>> WarmUpAsync() override;

private:
    std::shared_ptr<run::redis::RedisSentinelClient> m_client;
    std::shared_ptr<IHexOwnershipSource> m_fallback_source;
    ParentZone6Resolver m_parent_zone6_of;
};

} // namespace run::repository
