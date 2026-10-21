#pragma once

#include "storage/redis/i_batch_flusher.hpp"
#include "storage/redis/sentinel_config.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace run::storage::redis
{

namespace net = boost::asio;

/**
 * @class RedisConnectionPool
 * @brief High-throughput, multiplexed connection pool for Redis master nodes.
 *
 * ### Architectural Design:
 * 1. **Multiplexed Pipeline (Boost.Redis)**:
 *    Each individual `boost::redis::connection` operates as a fully asynchronous,
 *    pipelined TCP multiplexer over the RESP3 protocol. Multiple coroutines can queue
 *    commands simultaneously without waiting for prior responses; requests and responses
 *    are streamed over a single socket in FIFO sequence.
 *
 * 2. **Connection Pool Topology (2-4 connections per shard)**:
 *    To prevent head-of-line blocking on compute-heavy commands, eliminate TCP socket
 *    buffer contention under extreme GPS ingestion rates (10k+ points/sec), and scale
 *    across multiple Boost.Asio I/O threads/strands, this class maintains an active pool
 *    of N independent multiplexed connections (`pool_size`, default 2-4) to the resolved
 *    shard master. Requests are dispatched via atomic round-robin.
 *
 * 3. **Hot-Path TTL Throttling**:
 *    Instead of issuing redundant `EXPIRE` commands on every GPS point (which halves Redis
 *    throughput), TTL is renewed at a configurable interval (30-60 seconds) per active run,
 *    recording `last_ttl_refresh` in the `run:active:<run_id>` hash.
 */
class RedisConnectionPool : public std::enable_shared_from_this<RedisConnectionPool>,
                            public IBatchFlusher<DeferredTrackEvent>
{
public:
    explicit RedisConnectionPool(net::any_io_executor executor, RedisConfig config);
    ~RedisConnectionPool() override = default;

    RedisConnectionPool(const RedisConnectionPool&) = delete;
    RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

    void ConnectToMaster(const std::string& host, const std::string& port);
    void UpdateMaster(const std::string& new_host, const std::string& new_port);

    net::awaitable<bool> PingAsync();

    // Telemetry & Track
    net::awaitable<bool> PushTrackPointAsync(uint64_t run_id, double lat, double lon, double speed, int64_t ts);
    net::awaitable<bool> UpdateActiveRunStatsAsync(uint64_t run_id, double delta_meters, int32_t delta_points);
    net::awaitable<bool> FlushTrackBatchAsync(const std::vector<DeferredTrackEvent>& batch);

    // IBatchFlusher interface implementation
    net::awaitable<bool> FlushBatchAsync(const std::vector<DeferredTrackEvent>& batch) override
    {
        co_return co_await FlushTrackBatchAsync(batch);
    }

    // WebSocket Ticket
    net::awaitable<bool> SetWsTicketAsync(const std::string& ticket, uint64_t user_id,
                                          std::chrono::seconds ttl = std::chrono::seconds(30));
    net::awaitable<std::optional<uint64_t>> ValidateWsTicketAsync(const std::string& ticket);

    // Pub/Sub Publish
    net::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload);

    // Generic key-value helpers
    net::awaitable<bool> SetAsync(const std::string& key, const std::string& value, std::chrono::seconds ttl);

    /// SET NX EX: true = acquired, false = key existed (duplicate), nullopt = connectivity/command error
    net::awaitable<std::optional<bool>> SetNxAsync(const std::string& key, const std::string& value,
                                                   std::chrono::seconds ttl);
    net::awaitable<std::optional<std::string>> GetAsync(const std::string& key);
    net::awaitable<bool> DeleteAsync(const std::string& key);
    net::awaitable<bool> ExistsAsync(const std::string& key);

    // Generic Hash helpers
    net::awaitable<bool> HSetAsync(const std::string& key, const std::string& field, const std::string& value,
                                   std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<bool> HSetMultipleAsync(const std::string& key,
                                           const std::vector<std::pair<std::string, std::string>>& field_values,
                                           std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<bool> HSetMultipleAsync(const std::string& key,
                                           const std::vector<std::string>& flattened_field_values,
                                           std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<std::optional<std::string>> HGetAsync(const std::string& key, const std::string& field);
    net::awaitable<std::unordered_map<std::string, std::string>> HGetAllAsync(const std::string& key);

    // List helpers (full track retrieval for finish-time capture resolve)
    net::awaitable<std::vector<std::string>> LRangeAsync(const std::string& key, int64_t start, int64_t stop);

    // Observability and state queries
    bool IsConnected() const noexcept;
    std::pair<std::string, std::string> GetActiveMaster() const;
    std::size_t GetConnectionCount() const noexcept;
    std::size_t GetPoolSize() const noexcept;

    // TTL Optimization Helper
    bool ShouldRefreshTtl(uint64_t run_id);
    void InvalidateTtl(uint64_t run_id);

private:
    std::shared_ptr<boost::redis::connection> GetConnection() const;

private:
    net::any_io_executor m_executor;
    RedisConfig m_config;

    mutable std::mutex m_mutex;
    std::string m_current_host;
    std::string m_current_port;
    bool m_is_connected{false};

    // Pool of multiplexed Boost.Redis connections
    std::vector<std::shared_ptr<boost::redis::connection>> m_connections;
    mutable std::atomic<std::size_t> m_next_connection{0};

    // Hot-path TTL renewal throttle cache: run_id -> last_refresh_time
    mutable std::mutex m_ttl_mutex;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_last_ttl_refresh;
};

} // namespace run::storage::redis
