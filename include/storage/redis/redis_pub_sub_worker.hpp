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
    net::awaitable<void> RunSubscriberLoopAsync();
    void Reconnect(const std::string& host, const std::string& port);

private:
    net::any_io_executor m_executor;
    RedisConfig m_config;
    std::shared_ptr<SentinelMonitor> m_sentinel_monitor;
    std::string m_pattern;

    std::atomic<bool> m_is_running{false};
    mutable std::mutex m_mutex;
    MessageCallback m_callback;

    std::shared_ptr<boost::redis::connection> m_connection;
    std::string m_current_host;
    std::string m_current_port;
};

} // namespace run::storage::redis
