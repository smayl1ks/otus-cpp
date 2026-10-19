#include "storage/redis/redis_connection_pool.hpp"
#include "logger/logger.hpp"
#include <boost/redis/src.hpp>

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <charconv>
#include <fmt/format.h>

namespace run::storage::redis
{

RedisConnectionPool::RedisConnectionPool(net::any_io_executor executor, RedisConfig config)
    : m_executor(std::move(executor)), m_config(std::move(config))
{
}

void RedisConnectionPool::ConnectToMaster(const std::string& host, const std::string& port)
{
    std::lock_guard lock(m_mutex);

    if (m_connection && m_current_host == host && m_current_port == port)
    {
        return;
    }

    if (m_connection)
    {
        LOG_INFO("Closing previous connection to Redis master {}:{}", m_current_host, m_current_port);
        m_connection->cancel();
    }

    m_current_host = host;
    m_current_port = port;

    boost::redis::config cfg;
    cfg.addr.host = host;
    cfg.addr.port = port;
    if (!m_config.password.empty())
    {
        cfg.password = m_config.password;
    }
    cfg.reconnect_wait_interval = std::chrono::seconds(1);
    cfg.health_check_interval = std::chrono::seconds(5);

    m_connection = std::make_shared<boost::redis::connection>(m_executor);
    m_is_connected = true;

    LOG_INFO("Connecting to Redis master at {}:{}...", host, port);
    m_connection->async_run(cfg, net::consign(net::detached, m_connection));
}

void RedisConnectionPool::UpdateMaster(const std::string& new_host, const std::string& new_port)
{
    LOG_INFO("Updating active Redis master destination to {}:{}", new_host, new_port);
    ConnectToMaster(new_host, new_port);
}

bool RedisConnectionPool::IsConnected() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_is_connected && (m_connection != nullptr);
}

std::pair<std::string, std::string> RedisConnectionPool::GetActiveMaster() const
{
    std::lock_guard lock(m_mutex);
    return {m_current_host, m_current_port};
}

net::awaitable<bool> RedisConnectionPool::PingAsync()
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push("PING");

    boost::redis::response<std::string> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("Redis PING failed: {}", ec.message());
        co_return false;
    }

    co_return (std::get<0>(resp).value() == "PONG");
}

net::awaitable<bool> RedisConnectionPool::PushTrackPointAsync(uint64_t run_id, double lat, double lon, double speed,
                                                              int64_t ts)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    const std::string track_key = fmt::format("run:track:{}", run_id);
    const std::string point_str = fmt::format("{},{},{},{}", lat, lon, speed, ts);

    boost::redis::request req;
    req.push("RPUSH", track_key, point_str);
    req.push("EXPIRE", track_key, "86400");

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("Redis RPUSH track point failed for run {}: {}", run_id, ec.message());
        co_return false;
    }

    co_return true;
}

net::awaitable<bool> RedisConnectionPool::UpdateActiveRunStatsAsync(uint64_t run_id, double delta_meters,
                                                                    int32_t delta_points)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    const std::string active_key = fmt::format("run:active:{}", run_id);

    boost::redis::request req;
    req.push("HINCRBYFLOAT", active_key, "distance_meters", fmt::format("{:.2f}", delta_meters));
    if (delta_points > 0)
    {
        req.push("HINCRBY", active_key, "uram_points", std::to_string(delta_points));
    }
    req.push("EXPIRE", active_key, "86400");

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("Redis update active run stats failed for run {}: {}", run_id, ec.message());
        co_return false;
    }

    co_return true;
}

net::awaitable<bool> RedisConnectionPool::FlushTrackBatchAsync(const std::vector<DeferredTrackEvent>& batch)
{
    if (batch.empty())
    {
        co_return true;
    }

    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;

    for (const auto& ev : batch)
    {
        const std::string track_key = fmt::format("run:track:{}", ev.run_id);
        const std::string point_str = fmt::format("{},{},{},{}", ev.latitude, ev.longitude, ev.speed, ev.timestamp);
        req.push("RPUSH", track_key, point_str);
        req.push("EXPIRE", track_key, "86400");

        if (ev.delta_meters > 0.0 || ev.delta_points > 0)
        {
            const std::string active_key = fmt::format("run:active:{}", ev.run_id);
            if (ev.delta_meters > 0.0)
            {
                req.push("HINCRBYFLOAT", active_key, "distance_meters", fmt::format("{:.2f}", ev.delta_meters));
            }
            if (ev.delta_points > 0)
            {
                req.push("HINCRBY", active_key, "uram_points", std::to_string(ev.delta_points));
            }
            req.push("EXPIRE", active_key, "86400");
        }
    }

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    const auto timeout = std::max(m_config.command_timeout * 2, std::chrono::milliseconds(500));

    co_await conn->async_exec(req, resp, net::cancel_after(timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("Redis batch flush failed for {} events: {}", batch.size(), ec.message());
        co_return false;
    }

    LOG_DEBUG("Successfully flushed batch of {} deferred track events to Redis Master", batch.size());
    co_return true;
}

net::awaitable<bool> RedisConnectionPool::SetWsTicketAsync(const std::string& ticket, uint64_t user_id,
                                                           std::chrono::seconds ttl)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    const std::string key = fmt::format("ws_ticket:{}", ticket);

    boost::redis::request req;
    req.push("SET", key, std::to_string(user_id), "EX", std::to_string(ttl.count()));

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("Failed to set ws_ticket in Redis: {}", ec.message());
        co_return false;
    }

    co_return true;
}

net::awaitable<std::optional<uint64_t>> RedisConnectionPool::ValidateWsTicketAsync(const std::string& ticket)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn || ticket.empty())
    {
        co_return std::nullopt;
    }

    const std::string key = fmt::format("ws_ticket:{}", ticket);

    boost::redis::request req;
    req.push("GETDEL", key);

    boost::redis::response<std::optional<std::string>> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("Failed to validate ws_ticket in Redis: {}", ec.message());
        co_return std::nullopt;
    }

    const auto& opt_val = std::get<0>(resp);
    if (!opt_val.has_value() || !opt_val.value().has_value() || opt_val.value()->empty())
    {
        co_return std::nullopt;
    }

    uint64_t user_id = 0;
    const std::string& val_str = opt_val.value().value();
    auto [ptr, parse_ec] = std::from_chars(val_str.data(), val_str.data() + val_str.size(), user_id);
    if (parse_ec != std::errc{})
    {
        LOG_ERROR("Failed to parse user_id '{}' from ticket {}", val_str, ticket);
        co_return std::nullopt;
    }

    co_return user_id;
}

net::awaitable<bool> RedisConnectionPool::PublishAsync(const std::string& channel, const std::string& payload)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push("PUBLISH", channel, payload);

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("Failed to publish to channel {}: {}", channel, ec.message());
        co_return false;
    }

    co_return true;
}

net::awaitable<bool> RedisConnectionPool::SetAsync(const std::string& key, const std::string& value,
                                                   std::chrono::seconds ttl)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push("SET", key, value, "EX", std::to_string(ttl.count()));

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    co_return !ec;
}

net::awaitable<std::optional<std::string>> RedisConnectionPool::GetAsync(const std::string& key)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return std::nullopt;
    }

    boost::redis::request req;
    req.push("GET", key);

    boost::redis::response<std::optional<std::string>> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        co_return std::nullopt;
    }

    const auto& opt = std::get<0>(resp);
    if (!opt.has_value() || !opt.value().has_value())
    {
        co_return std::nullopt;
    }

    co_return opt.value().value();
}

net::awaitable<bool> RedisConnectionPool::DeleteAsync(const std::string& key)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push("DEL", key);

    boost::redis::response<int> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        co_return false;
    }

    const auto& val = std::get<0>(resp);
    co_return (val.has_value() && val.value() > 0);
}

net::awaitable<bool> RedisConnectionPool::ExistsAsync(const std::string& key)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push("EXISTS", key);

    boost::redis::response<int> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        co_return false;
    }

    const auto& val = std::get<0>(resp);
    co_return (val.has_value() && val.value() > 0);
}

net::awaitable<bool> RedisConnectionPool::HSetAsync(const std::string& key, const std::string& field,
                                                    const std::string& value, std::optional<std::chrono::seconds> ttl)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push("HSET", key, field, value);
    if (ttl.has_value())
    {
        req.push("EXPIRE", key, std::to_string(ttl->count()));
    }

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    co_return !ec;
}

net::awaitable<std::optional<std::string>> RedisConnectionPool::HGetAsync(const std::string& key,
                                                                          const std::string& field)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    if (!conn)
    {
        co_return std::nullopt;
    }

    boost::redis::request req;
    req.push("HGET", key, field);

    boost::redis::response<std::optional<std::string>> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        co_return std::nullopt;
    }

    const auto& opt = std::get<0>(resp);
    if (!opt.has_value() || !opt.value().has_value())
    {
        co_return std::nullopt;
    }

    co_return opt.value().value();
}

net::awaitable<std::unordered_map<std::string, std::string>> RedisConnectionPool::HGetAllAsync(const std::string& key)
{
    std::shared_ptr<boost::redis::connection> conn;
    {
        std::lock_guard lock(m_mutex);
        conn = m_connection;
    }

    std::unordered_map<std::string, std::string> result;
    if (!conn)
    {
        co_return result;
    }

    boost::redis::request req;
    req.push("HGETALL", key);

    boost::redis::response<std::vector<std::string>> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        co_return result;
    }

    const auto& vec_opt = std::get<0>(resp);
    if (vec_opt.has_value())
    {
        const auto& vec = vec_opt.value();
        for (std::size_t i = 0; i + 1 < vec.size(); i += 2)
        {
            result[vec[i]] = vec[i + 1];
        }
    }

    co_return result;
}

} // namespace run::storage::redis
