#pragma once

#include "storage/redis/sentinel_config.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace run::storage::redis
{

namespace net = boost::asio;

class SentinelMonitor : public std::enable_shared_from_this<SentinelMonitor>
{
public:
    using MasterChangedCallback = std::function<void(const std::string& new_host, const std::string& new_port)>;

    explicit SentinelMonitor(net::any_io_executor executor, RedisConfig config);
    ~SentinelMonitor() = default;

    SentinelMonitor(const SentinelMonitor&) = delete;
    SentinelMonitor& operator=(const SentinelMonitor&) = delete;

    net::awaitable<bool> ResolveMasterAsync();

    void StartMonitoring();
    void Stop();

    std::pair<std::string, std::string> GetCurrentMaster() const;

    void RegisterOnMasterChanged(MasterChangedCallback callback);

private:
    net::awaitable<void> MonitorLoopAsync();
    net::awaitable<bool> TryResolveFromSentinel(const SentinelNode& node);

private:
    net::any_io_executor m_executor;
    RedisConfig m_config;

    mutable std::mutex m_mutex;
    std::string m_master_host;
    std::string m_master_port;
    bool m_is_running{false};

    std::vector<MasterChangedCallback> m_callbacks;
    std::shared_ptr<boost::redis::connection> m_subscriber_conn;
};

} // namespace run::storage::redis