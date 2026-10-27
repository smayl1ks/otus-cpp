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

    if (!m_connections.empty() && m_current_host == host && m_current_port == port)
    {
        return;
    }

    if (!m_connections.empty())
    {
        LOG_INFO("Closing {} previous connection(s) to Redis master {}:{}", m_connections.size(), m_current_host,
                 m_current_port);
        for (auto& conn : m_connections)
        {
            if (conn)
            {
                conn->cancel();
            }
        }
        m_connections.clear();
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

    const std::size_t pool_size = std::max<std::size_t>(1, m_config.pool_size);
    m_connections.reserve(pool_size);

    LOG_INFO("Connecting pool of {} multiplexed connection(s) to Redis master at {}:{}...", pool_size, host, port);
    for (std::size_t i = 0; i < pool_size; ++i)
    {
        auto conn = std::make_shared<boost::redis::connection>(m_executor);
        conn->async_run(cfg, net::consign(net::detached, conn));
        m_connections.push_back(std::move(conn));
    }

    m_is_connected = true;
}

void RedisConnectionPool::UpdateMaster(const std::string& new_host, const std::string& new_port)
{
    LOG_INFO("Updating active Redis master destination to {}:{}", new_host, new_port);
    ConnectToMaster(new_host, new_port);
}

void RedisConnectionPool::Stop()
{
    std::lock_guard lock(m_mutex);
    m_is_connected = false;
    for (auto& conn : m_connections)
    {
        if (conn)
        {
            conn->cancel();
        }
    }
    m_connections.clear();
}

std::shared_ptr<boost::redis::connection> RedisConnectionPool::GetConnection() const
{
    std::lock_guard lock(m_mutex);
    if (m_connections.empty())
    {
        return nullptr;
    }
    const std::size_t idx = m_next_connection.fetch_add(1, std::memory_order_relaxed) % m_connections.size();
    return m_connections[idx];
}

bool RedisConnectionPool::IsConnected() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_is_connected && !m_connections.empty();
}

std::pair<std::string, std::string> RedisConnectionPool::GetActiveMaster() const
{
    std::lock_guard lock(m_mutex);
    return {m_current_host, m_current_port};
}

std::size_t RedisConnectionPool::GetConnectionCount() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_connections.size();
}

std::size_t RedisConnectionPool::GetPoolSize() const noexcept
{
    return m_config.pool_size;
}

bool RedisConnectionPool::ShouldRefreshTtl(uint64_t run_id)
{
    const auto now = std::chrono::steady_clock::now();
    const auto interval =
        m_config.ttl_refresh_interval.count() > 0 ? m_config.ttl_refresh_interval : std::chrono::seconds(30);

    std::lock_guard lock(m_ttl_mutex);
    auto it = m_last_ttl_refresh.find(run_id);
    if (it == m_last_ttl_refresh.end() || (now - it->second) >= interval)
    {
        m_last_ttl_refresh[run_id] = now;

        if (m_last_ttl_refresh.size() > 20000)
        {
            for (auto iter = m_last_ttl_refresh.begin(); iter != m_last_ttl_refresh.end();)
            {
                if (now - iter->second > std::chrono::minutes(15))
                {
                    iter = m_last_ttl_refresh.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }

        return true;
    }
    return false;
}

void RedisConnectionPool::InvalidateTtl(uint64_t run_id)
{
    std::lock_guard lock(m_ttl_mutex);
    m_last_ttl_refresh.erase(run_id);
}

net::awaitable<bool> RedisConnectionPool::PingAsync()
{
    auto conn = GetConnection();
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
    auto conn = GetConnection();
    if (!conn)
    {
        co_return false;
    }

    const std::string track_key = fmt::format("run:track:{}", run_id);
    const std::string point_str = fmt::format("{},{},{},{}", lat, lon, speed, ts);

    boost::redis::request req;
    req.push("RPUSH", track_key, point_str);

    // Optimized: Only refresh TTL at configured interval (30-60s) instead of per point
    if (ShouldRefreshTtl(run_id))
    {
        req.push("EXPIRE", track_key, "86400");
        const std::string active_key = fmt::format("run:active:{}", run_id);
        req.push("EXPIRE", active_key, "86400");
        const auto now_sec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        req.push("HSET", active_key, "last_ttl_refresh", std::to_string(now_sec));
    }

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
    auto conn = GetConnection();
    if (!conn)
    {
        co_return false;
    }

    const std::string active_key = fmt::format("run:active:{}", run_id);

    boost::redis::request req;
    if (delta_meters > 0.0)
    {
        req.push("HINCRBYFLOAT", active_key, "distance_meters", fmt::format("{:.2f}", delta_meters));
    }
    if (delta_points > 0)
    {
        req.push("HINCRBY", active_key, "points_count", std::to_string(delta_points));
    }

    // Refresh TTL periodically if due
    if (ShouldRefreshTtl(run_id))
    {
        req.push("EXPIRE", active_key, "86400");
        const std::string track_key = fmt::format("run:track:{}", run_id);
        req.push("EXPIRE", track_key, "86400");
        const auto now_sec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        req.push("HSET", active_key, "last_ttl_refresh", std::to_string(now_sec));
    }

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

    auto conn = GetConnection();
    if (!conn)
    {
        co_return false;
    }

    struct RunBatchAggregate
    {
        uint64_t numeric_run_id{0};
        std::vector<std::string> points;
        double total_delta_meters{0.0};
        int32_t total_delta_points{0};
        int64_t last_sequence_number{0};
    };

    std::unordered_map<std::string, RunBatchAggregate> grouped;
    for (const auto& ev : batch)
    {
        const std::string id_str = (ev.session_id[0] != '\0') ? std::string(ev.session_id) : std::to_string(ev.run_id);
        auto& data = grouped[id_str];
        data.numeric_run_id = ev.run_id;
        data.points.push_back(fmt::format("{},{},{},{}", ev.latitude, ev.longitude, ev.speed, ev.timestamp));
        data.total_delta_meters += ev.delta_meters;
        data.total_delta_points += ev.delta_points;
        data.last_sequence_number = std::max(data.last_sequence_number, ev.sequence_number);
    }

    boost::redis::request req;
    const auto now_sec =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto& [id_str, data] : grouped)
    {
        const std::string track_key = fmt::format("run:track:{}", id_str);
        const std::string active_key = fmt::format("run:active:{}", id_str);

        req.push_range("RPUSH", track_key, data.points.begin(), data.points.end());

        if (data.total_delta_meters > 0.0)
        {
            req.push("HINCRBYFLOAT", active_key, "total_distance_meters", fmt::format("{:.2f}", data.total_delta_meters));
            req.push("HINCRBYFLOAT", active_key, "distance_meters", fmt::format("{:.2f}", data.total_delta_meters));
        }
        if (data.total_delta_points > 0)
        {
            req.push("HINCRBY", active_key, "total_points_count", std::to_string(data.total_delta_points));
            req.push("HINCRBY", active_key, "points_count", std::to_string(data.total_delta_points));
        }
        if (data.last_sequence_number > 0)
        {
            req.push("HSET", active_key, "last_sequence_number", std::to_string(data.last_sequence_number));
        }

        req.push("EXPIRE", track_key, "86400");
        req.push("EXPIRE", active_key, "86400");
        req.push("HSET", active_key, "last_ttl_refresh", std::to_string(now_sec));
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

    LOG_DEBUG("Successfully flushed batch of {} deferred track events to Redis Master in single async_exec",
              batch.size());
    co_return true;
}

net::awaitable<bool> RedisConnectionPool::SetWsTicketAsync(const std::string& ticket, uint64_t user_id,
                                                           std::chrono::seconds ttl)
{
    auto conn = GetConnection();
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
    auto conn = GetConnection();
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
    auto conn = GetConnection();
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
    auto conn = GetConnection();
    if (!conn)
    {
        LOG_WARN("SET {} failed: no active connection in pool", key);
        co_return false;
    }

    boost::redis::request req;
    req.push("SET", key, value, "EX", std::to_string(ttl.count()));

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("SET {} failed: {}", key, ec.message());
        co_return false;
    }

    co_return true;
}

net::awaitable<std::optional<bool>> RedisConnectionPool::SetNxAsync(const std::string& key,
                                                                     const std::string& value,
                                                                     std::chrono::seconds ttl)
{
    auto conn = GetConnection();
    if (!conn)
    {
        co_return std::nullopt;
    }

    boost::redis::request req;
    req.push("SET", key, value, "EX", std::to_string(ttl.count()), "NX");

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("SETNX {} failed: {}", key, ec.message());
        co_return std::nullopt;
    }

    // SET NX replies +OK on acquire and nil when the key already exists
    const bool acquired = !resp.value().empty() && !resp.value().front().value.empty();
    co_return acquired;
}

net::awaitable<std::optional<std::string>> RedisConnectionPool::GetAsync(const std::string& key)
{
    auto conn = GetConnection();
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
    auto conn = GetConnection();
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
    auto conn = GetConnection();
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
    auto conn = GetConnection();
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

net::awaitable<bool>
RedisConnectionPool::HSetMultipleAsync(const std::string& key,
                                       const std::vector<std::pair<std::string, std::string>>& field_values,
                                       std::optional<std::chrono::seconds> ttl)
{
    std::vector<std::string> flattened;
    flattened.reserve(field_values.size() * 2);
    for (const auto& [f, v] : field_values)
    {
        flattened.push_back(f);
        flattened.push_back(v);
    }

    co_return co_await HSetMultipleAsync(key, flattened, ttl);
}

net::awaitable<bool> RedisConnectionPool::HSetMultipleAsync(const std::string& key,
                                                            const std::vector<std::string>& flattened_field_values,
                                                            std::optional<std::chrono::seconds> ttl)
{
    auto conn = GetConnection();
    if (!conn || flattened_field_values.empty())
    {
        co_return false;
    }

    boost::redis::request req;
    req.push_range("HSET", key, flattened_field_values.begin(), flattened_field_values.end());
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
    auto conn = GetConnection();
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
    auto conn = GetConnection();
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

net::awaitable<std::vector<std::string>> RedisConnectionPool::LRangeAsync(const std::string& key, int64_t start,
                                                                          int64_t stop)
{
    std::vector<std::string> result;
    auto conn = GetConnection();
    if (!conn)
    {
        co_return result;
    }

    boost::redis::request req;
    req.push("LRANGE", key, std::to_string(start), std::to_string(stop));

    boost::redis::response<std::vector<std::string>> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("LRANGE {} failed: {}", key, ec.message());
        co_return result;
    }

    if (const auto& vec_opt = std::get<0>(resp); vec_opt.has_value())
    {
        result = std::move(vec_opt.value());
    }

    co_return result;
}

net::awaitable<bool> RedisConnectionPool::RPushRangeAsync(const std::string& key,
                                                           const std::vector<std::string>& values,
                                                           std::optional<std::chrono::seconds> ttl)
{
    if (values.empty())
    {
        co_return true;
    }
    auto conn = GetConnection();
    if (!conn)
    {
        co_return false;
    }

    boost::redis::request req;
    req.push_range("RPUSH", key, values.begin(), values.end());
    if (ttl.has_value())
    {
        req.push("EXPIRE", key, std::to_string(ttl->count()));
    }

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("RPUSH {} failed: {}", key, ec.message());
        co_return false;
    }
    co_return true;
}

net::awaitable<int64_t> RedisConnectionPool::SAddAsync(const std::string& key, const std::string& member,
                                                      std::optional<std::chrono::seconds> ttl)
{
    auto conn = GetConnection();
    if (!conn)
    {
        co_return -1;
    }

    boost::redis::request req;
    req.push("SADD", key, member);
    if (ttl.has_value())
    {
        req.push("EXPIRE", key, std::to_string(ttl->count()));
    }

    boost::redis::generic_response resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("SADD {} failed: {}", key, ec.message());
        co_return -1;
    }

    if (resp.has_value() && !resp.value().empty())
    {
        try
        {
            co_return std::stoll(resp.value().front().value);
        }
        catch (...)
        {
            co_return 1;
        }
    }
    co_return 0;
}

net::awaitable<std::vector<std::string>> RedisConnectionPool::SMembersAsync(const std::string& key)
{
    std::vector<std::string> result;
    auto conn = GetConnection();
    if (!conn)
    {
        co_return result;
    }

    boost::redis::request req;
    req.push("SMEMBERS", key);

    boost::redis::response<std::vector<std::string>> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("SMEMBERS {} failed: {}", key, ec.message());
        co_return result;
    }

    if (const auto& vec_opt = std::get<0>(resp); vec_opt.has_value())
    {
        result = std::move(vec_opt.value());
    }
    co_return result;
}

net::awaitable<int64_t> RedisConnectionPool::SCardAsync(const std::string& key)
{
    auto conn = GetConnection();
    if (!conn)
    {
        co_return 0;
    }

    boost::redis::request req;
    req.push("SCARD", key);

    boost::redis::response<int64_t> resp;
    boost::system::error_code ec;

    co_await conn->async_exec(req, resp,
                              net::cancel_after(m_config.command_timeout, net::redirect_error(net::use_awaitable, ec)));

    if (ec)
    {
        LOG_WARN("SCARD {} failed: {}", key, ec.message());
        co_return 0;
    }

    if (const auto& res_opt = std::get<0>(resp); res_opt.has_value())
    {
        co_return res_opt.value();
    }
    co_return 0;
}

} // namespace run::storage::redis
