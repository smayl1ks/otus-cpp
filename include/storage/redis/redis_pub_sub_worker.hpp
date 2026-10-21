#pragma once

#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "storage/redis/sentinel_config.hpp"
#include "storage/redis/sentinel_monitor.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace run::storage::redis
{

namespace net = boost::asio;

class RedisPubSubWorker : public std::enable_shared_from_this<RedisPubSubWorker>
{
public:
    using MessageCallback = std::function<void(std::string_view channel, std::string_view payload)>;

    RedisPubSubWorker(net::any_io_executor executor, RedisConfig config,
                      std::shared_ptr<SentinelMonitor> sentinel_monitor, std::string pattern = "kazan:zone:*");
    ~RedisPubSubWorker() = default;

    RedisPubSubWorker(const RedisPubSubWorker&) = delete;
    RedisPubSubWorker& operator=(const RedisPubSubWorker&) = delete;

    void Start();
    void Stop();

    void SetMessageHandler(MessageCallback callback);
    void SetPattern(std::string pattern);
    const std::string& GetPattern() const noexcept;

    static uint64_t ExtractH3Zone(std::string_view channel_name, std::string_view prefix = "kazan:zone:")
    {
        return gameplay::territory_conquest::ZoneChannelHelper::ExtractH3Zone(channel_name, prefix);
    }

    static std::string FormatZoneChannel(uint64_t h3_zone, std::string_view prefix = "kazan")
    {
        return gameplay::territory_conquest::ZoneChannelHelper::FormatChannel(h3_zone, prefix);
    }

private:
    /**
     * @brief Resolves the address list to subscribe to.
     * Fleet (pubsub_hosts) > single instance (pubsub_host) > Sentinel master.
     * Each fleet instance gets its own subscriber loop.
     */
    std::vector<std::pair<std::string, std::string>> ResolveAddresses() const;

    net::awaitable<void> RunSubscriberLoopAsync(std::string host, std::string port);
    void Reconnect(const std::string& host, const std::string& port);

private:
    net::any_io_executor m_executor;
    RedisConfig m_config;
    std::shared_ptr<SentinelMonitor> m_sentinel_monitor;
    std::string m_pattern;

    std::atomic<bool> m_is_running{false};
    mutable std::mutex m_mutex;
    MessageCallback m_callback;

    std::vector<std::shared_ptr<boost::redis::connection>> m_connections;
};

} // namespace run::storage::redis
