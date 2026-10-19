
#include "storage/redis/redis_pub_sub_worker.hpp"
#include "logger/logger.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace run::storage::redis
{

RedisPubSubWorker::RedisPubSubWorker(net::any_io_executor executor, RedisConfig config,
                                     std::shared_ptr<SentinelMonitor> sentinel_monitor, std::string pattern)
    : m_executor(executor), m_config(std::move(config)), m_sentinel_monitor(std::move(sentinel_monitor)),
      m_pattern(std::move(pattern))
{
    if (m_sentinel_monitor)
    {
        m_sentinel_monitor->RegisterOnMasterChanged([this](const std::string& new_host, const std::string& new_port)
                                                    { Reconnect(new_host, new_port); });
    }
}

void RedisPubSubWorker::Start()
{
    std::lock_guard lock(m_mutex);
    if (m_is_running)
    {
        return;
    }
    m_is_running = true;

    net::co_spawn(m_executor, RunSubscriberLoopAsync(), net::detached);
}

void RedisPubSubWorker::Stop()
{
    m_is_running = false;
    std::lock_guard lock(m_mutex);
    if (m_connection)
    {
        m_connection->cancel();
    }
}

void RedisPubSubWorker::SetMessageHandler(MessageCallback callback)
{
    std::lock_guard lock(m_mutex);
    m_callback = std::move(callback);
}

void RedisPubSubWorker::SetPattern(std::string pattern)
{
    std::lock_guard lock(m_mutex);
    m_pattern = std::move(pattern);
    if (m_connection)
    {
        m_connection->cancel();
    }
}

const std::string& RedisPubSubWorker::GetPattern() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_pattern;
}

void RedisPubSubWorker::Reconnect(const std::string& host, const std::string& port)
{
    LOG_INFO("RedisPubSubWorker: Sentinel master failover to {}:{}. Re-subscribing...", host, port);
    std::lock_guard lock(m_mutex);
    m_current_host = host;
    m_current_port = port;
    if (m_connection)
    {
        m_connection->cancel();
    }
}

net::awaitable<void> RedisPubSubWorker::RunSubscriberLoopAsync()
{
    while (m_is_running)
    {
        std::string host;
        std::string port;
        std::string pattern;
        {
            std::lock_guard lock(m_mutex);
            host = m_current_host;
            port = m_current_port;
            pattern = m_pattern;
        }

        if (host.empty() && m_sentinel_monitor)
        {
            const auto [m_host, m_port] = m_sentinel_monitor->GetCurrentMaster();
            host = m_host;
            port = m_port;
        }

        if (host.empty() || port.empty())
        {
            LOG_DEBUG("RedisPubSubWorker: Waiting for active master address...");
            net::steady_timer timer(co_await net::this_coro::executor);
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(net::use_awaitable);
            continue;
        }

        auto conn = std::make_shared<boost::redis::connection>(m_executor);
        {
            std::lock_guard lock(m_mutex);
            m_connection = conn;
        }

        boost::redis::config cfg;
        cfg.addr.host = host;
        cfg.addr.port = port;
        if (!m_config.password.empty())
        {
            cfg.password = m_config.password;
        }

        conn->async_run(cfg, net::consign(net::detached, conn));

        boost::redis::request req;
        req.push("PSUBSCRIBE", pattern);

        boost::redis::generic_response sub_resp;
        boost::system::error_code ec;

        co_await conn->async_exec(req, sub_resp, net::redirect_error(net::use_awaitable, ec));

        if (ec)
        {
            LOG_WARN("RedisPubSubWorker failed to subscribe to pattern '{}' on {}:{}: {}", pattern, host, port,
                     ec.message());
            conn->cancel();

            net::steady_timer timer(co_await net::this_coro::executor);
            timer.expires_after(std::chrono::seconds(2));
            co_await timer.async_wait(net::use_awaitable);
            continue;
        }

        LOG_INFO("RedisPubSubWorker successfully subscribed to pattern '{}' on {}:{}", pattern, host, port);

        boost::redis::generic_response push_resp;
        conn->set_receive_response(push_resp);

        while (m_is_running)
        {
            push_resp.value().clear();

            auto [rec_ec, n] = co_await conn->async_receive(net::as_tuple(net::use_awaitable));
            if (rec_ec)
            {
                LOG_WARN("RedisPubSubWorker receive error on {}:{}: {}", host, port, rec_ec.message());
                break;
            }

            if (!push_resp.has_value())
            {
                continue;
            }

            const auto& nodes = push_resp.value();
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                // pmessage format: [pmessage, pattern, channel, payload]
                if (nodes[i].value == "pmessage" && i + 3 < nodes.size())
                {
                    const std::string_view channel = nodes[i + 2].value;
                    const std::string_view payload = nodes[i + 3].value;

                    MessageCallback cb;
                    {
                        std::lock_guard lock(m_mutex);
                        cb = m_callback;
                    }

                    if (cb)
                    {
                        cb(channel, payload);
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
