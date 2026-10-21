#pragma once

#include "storage/redis/sentinel_config.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace run::storage::redis
{

namespace net = boost::asio;

/**
 * @class RedisPublisher
 * @brief Dedicated publish-only connection to the standalone Pub/Sub instance.
 *
 * Radar traffic (kazan:zone:* PUBLISH fan-out) is routed to a dedicated Redis
 * instance so it never competes with hot-path telemetry writes on the sharded
 * masters. Pub/Sub is fire-and-forget, so this class deliberately has no
 * circuit breaker or deferred buffer: if the instance is unreachable, pings
 * are dropped (they are only meaningful for currently-connected clients).
 *
 * When pubsub_host is empty in RedisConfig, the publisher is unconfigured and
 * callers fall back to the legacy shard-based PublishAsync path.
 */
class RedisPublisher : public std::enable_shared_from_this<RedisPublisher>
{
public:
    RedisPublisher(net::any_io_executor executor, RedisConfig config);
    ~RedisPublisher() = default;

    RedisPublisher(const RedisPublisher&) = delete;
    RedisPublisher& operator=(const RedisPublisher&) = delete;

    void Start();
    void Stop();

    net::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload);

    bool IsConfigured() const noexcept;
    bool IsConnected() const noexcept;
    std::pair<std::string, std::string> GetAddress() const noexcept;

private:
    net::awaitable<void> RunLoopAsync();
    std::shared_ptr<boost::redis::connection> GetConnection() const;

private:
    net::any_io_executor m_executor;
    RedisConfig m_config;

    std::atomic<bool> m_is_running{false};
    std::atomic<bool> m_is_connected{false};

    mutable std::mutex m_mutex;
    std::shared_ptr<boost::redis::connection> m_connection;
};

} // namespace run::storage::redis
