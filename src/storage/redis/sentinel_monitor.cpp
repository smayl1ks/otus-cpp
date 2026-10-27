#include "storage/redis/sentinel_monitor.hpp"
#include "logger/logger.hpp"
#include "monitoring/metrics_collector.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <sstream>

namespace run::storage::redis
{

SentinelMonitor::SentinelMonitor(net::any_io_executor executor, RedisConfig config)
    : m_executor(std::move(executor)), m_config(std::move(config))
{
}

net::awaitable<bool> SentinelMonitor::ResolveMasterAsync()
{
    if (m_config.sentinels.empty())
    {
        LOG_WARN("No sentinels configured for master '{}'", m_config.master_name);
        co_return false;
    }

    for (const auto& sentinel_node : m_config.sentinels)
    {
        if (co_await TryResolveFromSentinel(sentinel_node))
        {
            co_return true;
        }
    }

    LOG_ERROR("Failed to resolve master '{}' from all {} sentinel node(s)", m_config.master_name,
              m_config.sentinels.size());
    co_return false;
}

net::awaitable<bool> SentinelMonitor::TryResolveFromSentinel(const SentinelNode& node)
{
    LOG_DEBUG("Attempting to resolve Redis master from Sentinel {}:{}", node.host, node.port);

    auto conn = std::make_shared<boost::redis::connection>(m_executor);
    boost::redis::config cfg;
    cfg.addr.host = node.host;
    cfg.addr.port = node.port;

    boost::system::error_code ec;

    conn->async_run(cfg, net::consign(net::detached, conn));

    boost::redis::request req;
    req.push("SENTINEL", "get-master-addr-by-name", m_config.master_name);

    boost::redis::response<std::optional<std::vector<std::string>>> resp;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(std::chrono::seconds(2), net::redirect_error(net::use_awaitable, ec)));

    conn->cancel();

    if (ec)
    {
        LOG_WARN("Sentinel {}:{} error during master discovery: {}", node.host, node.port, ec.message());
        co_return false;
    }

    const auto& res = std::get<0>(resp);
    if (!res.has_value() || !res.value().has_value() || res.value().value().size() < 2)
    {
        LOG_WARN("Sentinel {}:{} returned invalid response for master '{}'", node.host, node.port,
                 m_config.master_name);
        co_return false;
    }

    const auto& vec = res.value().value();
    const std::string host = vec[0];
    const std::string port = vec[1];

    {
        std::lock_guard lock(m_mutex);
        m_master_host = host;
        m_master_port = port;
    }

    LOG_INFO("Successfully resolved Redis master '{}' -> {}:{} via Sentinel {}:{}", m_config.master_name, host, port,
             node.host, node.port);

    co_return true;
}

void SentinelMonitor::StartMonitoring()
{
    std::lock_guard lock(m_mutex);
    if (m_is_running)
    {
        return;
    }
    m_is_running = true;

    net::co_spawn(m_executor, MonitorLoopAsync(), net::detached);
}

void SentinelMonitor::Stop()
{
    std::lock_guard lock(m_mutex);
    m_is_running = false;
    if (m_subscriber_conn)
    {
        m_subscriber_conn->cancel();
    }
}

std::pair<std::string, std::string> SentinelMonitor::GetCurrentMaster() const
{
    std::lock_guard lock(m_mutex);
    return {m_master_host, m_master_port};
}

void SentinelMonitor::RegisterOnMasterChanged(MasterChangedCallback callback)
{
    std::lock_guard lock(m_mutex);
    m_callbacks.push_back(std::move(callback));
}

net::awaitable<void> SentinelMonitor::MonitorLoopAsync()
{
    size_t current_sentinel_idx = 0;

    while (m_is_running && !m_config.sentinels.empty())
    {
        const auto& node = m_config.sentinels[current_sentinel_idx % m_config.sentinels.size()];
        current_sentinel_idx++;

        LOG_INFO("Connecting to Sentinel {}:{} to watch for failover events (+switch-master)...", node.host, node.port);

        auto conn = std::make_shared<boost::redis::connection>(m_executor);
        {
            std::lock_guard lock(m_mutex);
            m_subscriber_conn = conn;
        }

        boost::redis::config cfg;
        cfg.addr.host = node.host;
        cfg.addr.port = node.port;

        conn->async_run(cfg, net::consign(net::detached, conn));

        boost::redis::request req;
        req.push("SUBSCRIBE", "+switch-master");

        boost::redis::generic_response sub_resp;
        boost::system::error_code ec;

        co_await conn->async_exec(req, sub_resp, net::redirect_error(net::use_awaitable, ec));

        if (ec)
        {
            LOG_WARN("Failed to subscribe to Sentinel {}:{} (+switch-master): {}", node.host, node.port, ec.message());
            conn->cancel();

            net::steady_timer timer(co_await net::this_coro::executor);
            timer.expires_after(std::chrono::seconds(2));
            co_await timer.async_wait(net::use_awaitable);
            continue;
        }

        LOG_INFO("Listening for Sentinel failover events on {}:{}...", node.host, node.port);

        boost::redis::generic_response push_resp;
        conn->set_receive_response(push_resp);

        while (m_is_running)
        {
            push_resp.value().clear();

            auto [rec_ec, n] = co_await conn->async_receive(net::as_tuple(net::use_awaitable));
            if (rec_ec)
            {
                LOG_WARN("Sentinel listener disconnected from {}:{}: {}", node.host, node.port, rec_ec.message());
                break;
            }

            if (!push_resp.has_value())
            {
                continue;
            }

            const auto& nodes = push_resp.value();
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                // push notification format: [message, +switch-master, payload]
                if (nodes[i].value == "message" && i + 2 < nodes.size())
                {
                    const std::string& channel = nodes[i + 1].value;
                    const std::string& payload = nodes[i + 2].value;

                    if (channel == "+switch-master")
                    {
                        // Payload format: <master-name> <old-ip> <old-port> <new-ip> <new-port>
                        std::stringstream ss(payload);
                        std::string master_name, old_ip, old_port, new_ip, new_port;
                        ss >> master_name >> old_ip >> old_port >> new_ip >> new_port;

                        if (master_name == m_config.master_name && !new_ip.empty() && !new_port.empty())
                        {
                            LOG_WARN(">>> SENTINEL FAILOVER DETECTED: Master '{}' moved from {}:{} to {}:{} <<<",
                                     master_name, old_ip, old_port, new_ip, new_port);
                            monitoring::MetricsCollector::Instance().IncrementSentinelFailover(master_name);

                            std::vector<MasterChangedCallback> cbs;
                            {
                                std::lock_guard lock(m_mutex);
                                m_master_host = new_ip;
                                m_master_port = new_port;
                                cbs = m_callbacks;
                            }

                            for (const auto& cb : cbs)
                            {
                                if (cb)
                                {
                                    cb(new_ip, new_port);
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }

        conn->cancel();

        if (m_is_running)
        {
            net::steady_timer timer(co_await net::this_coro::executor);
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(net::use_awaitable);
        }
    }
}

} // namespace run::storage::redis