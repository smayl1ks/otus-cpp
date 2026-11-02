#pragma once

#include "repository/i_hex_map_cache.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

#include <memory>

namespace run::storage::redis
{

// IHexMapCache over the sharded Redis master. Key layout:
//   map:zone:<h6:x>:hexes  — SET of captured res-9 hex indexes (lowercase hex)
//   map:hex:<h9:x>         — HASH owner_user_id/owner_username/owner_color_hex/score
// Every write refreshes a 30-day TTL on both keys as a safety net: the
// authoritative state is PostgreSQL, the cache only needs to outlive the
// gap between captures.
class RedisHexMapCache final : public repository::IHexMapCache
{
public:
    explicit RedisHexMapCache(std::shared_ptr<ShardedRedisManager> redis);

    boost::asio::awaitable<bool> WriteAsync(const repository::HexOwnershipRecord& hex, uint64_t parent_zone6) override;

    boost::asio::awaitable<bool>
    WriteZoneBatchAsync(std::span<const repository::HexOwnershipRecord> hexes, uint64_t parent_zone6) override;

    boost::asio::awaitable<std::vector<repository::HexOwnershipRecord>>
    GetSnapshotAsync(std::span<const uint64_t> parent_zones6) override;

private:
    std::shared_ptr<ShardedRedisManager> m_redis;
};

} // namespace run::storage::redis
