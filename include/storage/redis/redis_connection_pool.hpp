#pragma once

#include "storage/redis/i_batch_flusher.hpp"
#include "storage/redis/sentinel_config.hpp"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace run::storage::redis
{

namespace net = boost::asio;

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

    // WebSecket Ticket
    net::awaitable<bool> SetWsTicketAsync(const std::string& ticket, uint64_t user_id,
                                          std::chrono::seconds ttl = std::chrono::seconds(30));
    net::awaitable<std::optional<uint64_t>> ValidateWsTicketAsync(const std::string& ticket);

    // Pub/Sub Publish
    net::awaitable<bool> PublishAsync(const std::string& channel, const std::string& payload);

    // Generic key-value helpers
    net::awaitable<bool> SetAsync(const std::string& key, const std::string& value, std::chrono::seconds ttl);
    net::awaitable<std::optional<std::string>> GetAsync(const std::string& key);
    net::awaitable<bool> DeleteAsync(const std::string& key);
    net::awaitable<bool> ExistsAsync(const std::string& key);

    // Generic Hash helpers
    net::awaitable<bool> HSetAsync(const std::string& key, const std::string& field, const std::string& value,
                                   std::optional<std::chrono::seconds> ttl = std::nullopt);
    net::awaitable<std::optional<std::string>> HGetAsync(const std::string& key, const std::string& field);
    net::awaitable<std::unordered_map<std::string, std::string>> HGetAllAsync(const std::string& key);

    bool IsConnected() const noexcept;
    std::pair<std::string, std::string> GetActiveMaster() const;

private:
    net::any_io_executor m_executor;
    RedisConfig m_config;

    mutable std::mutex m_mutex;
    std::string m_current_host;
    std::string m_current_port;
    bool m_is_connected{false};

    std::shared_ptr<boost::redis::connection> m_connection;
};

} // namespace run::storage::redis
