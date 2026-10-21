#include "storage/redis/redis_publisher.hpp"
#include "logger/logger.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

namespace run::storage::redis
{

namespace
{
std::pair<std::string, std::string> ResolvePubSubAddress(const RedisConfig& config)
{
    if (!config.pubsub_host.empty())
    {
        return {config.pubsub_host,
                config.pubsub_port.empty() ? std::string{"6379"} : config.pubsub_port};
    }
    return {};
}

std::string ResolvePubSubPassword(const RedisConfig& config)
{
    return config.pubsub_password.empty() ? config.password : config.pubsub_password;
}
} // namespace

RedisPublisher::RedisPublisher(net::any_io_executor executor, RedisConfig config)
    : m_executor(std::move(executor)), m_config(std::move(config))
{
}

bool RedisPublisher::IsConfigured() const noexcept
{
    return !m_config.pubsub_host.empty();
}

bool RedisPublisher::IsConnected() const noexcept
{
    return m_is_connected.load(std::memory_order_relaxed);
}

std::pair<std::string, std::string> RedisPublisher::GetAddress() const noexcept
{
    return ResolvePubSubAddress(m_config);
}

void RedisPublisher::Start()
{
    if (!IsConfigured())
    {
        LOG_INFO("RedisPublisher: no dedicated pub/sub instance configured, radar publish will use shard fallback");
        return;
    }

    bool expected = false;
    if (!m_is_running.compare_exchange_strong(expected, true))
    {
        return;
    }

    LOG_INFO("RedisPublisher: starting dedicated pub/sub publisher for {}:{}", m_config.pubsub_host,
             m_config.pubsub_port);
    net::co_spawn(m_executor, RunLoopAsync(), net::detached);
}

void RedisPublisher::Stop()
{
    m_is_running.store(false);
    std::lock_guard lock(m_mutex);
    if (m_connection)
    {
        m_connection->cancel();
        m_connection.reset();
    }
    m_is_connected.store(false);
}

net::awaitable<bool> RedisPublisher::PublishAsync(const std::string& channel, const std::string& payload)
{
    auto conn = GetConnection();
    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push("PUBLISH", channel, payload);

    boost::redis::generic_response resp;
    boost::system::error_code ec;
    co_await conn->async_exec(req, resp, net::redirect_error(net::use_awaitable, ec));

    if (ec)
    {
        LOG_WARN("RedisPublisher: PUBLISH to '{}' on {} failed: {}", channel, m_config.pubsub_host, ec.message());
        co_return false;
    }

    co_return true;
}

std::shared_ptr<boost::redis::connection> RedisPublisher::GetConnection() const
{
    std::lock_guard lock(m_mutex);
    return m_connection;
}

net::awaitable<void> RedisPublisher::RunLoopAsync()
{
    while (m_is_running)
    {
        auto conn = std::make_shared<boost::redis::connection>(m_executor);
        {
            std::lock_guard lock(m_mutex);
            m_connection = conn;
        }

        boost::redis::config cfg;
        cfg.addr.host = m_config.pubsub_host;
        cfg.addr.port = m_config.pubsub_port.empty() ? "6379" : m_config.pubsub_port;
        cfg.health_check_interval = std::chrono::seconds(5);
        cfg.reconnect_wait_interval = std::chrono::seconds(1);
        if (const auto password = ResolvePubSubPassword(m_config); !password.empty())
        {
            cfg.password = password;
        }

        LOG_INFO("RedisPublisher: connecting to pub/sub instance at {}:{}", cfg.addr.host, cfg.addr.port);

        // Optimistic: boost-redis reconnects transparently inside async_run;
        // PublishAsync failures are the real connectivity signal.
        m_is_connected.store(true);

        boost::system::error_code run_ec;
        co_await conn->async_run(cfg, net::redirect_error(net::use_awaitable, run_ec));
        // async_run returns when the connection is lost or cancelled

        m_is_connected.store(false);
        conn->cancel();
        {
            std::lock_guard lock(m_mutex);
            if (m_connection == conn)
            {
                m_connection.reset();
            }
        }

        if (!m_is_running)
        {
            co_return;
        }

        LOG_WARN("RedisPublisher: connection to pub/sub instance lost ({}). Retrying in 2s...", run_ec.message());
        net::steady_timer timer(co_await net::this_coro::executor);
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(net::use_awaitable);
    }
}

} // namespace run::storage::redis
