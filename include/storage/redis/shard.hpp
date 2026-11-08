#pragma once

#include <boost/asio.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <chrono>
#include <memory>

#include "storage/redis/redis_config.hpp"
#include "storage/redis/sentinel_config.hpp"

namespace run::redis
{

class Shard
{
public:
    explicit Shard(boost::asio::any_io_executor& io_context, const SentinelConfig& redis_config,
                   std::size_t shard_index, const boost::redis::request* setup_commands = nullptr)
        : m_strand{boost::asio::make_strand(io_context)},
          m_connection{std::make_shared<boost::redis::connection>(m_strand)}, m_shard_index{shard_index},
          m_shard_master_name{MakeMasterName(redis_config.master_prefix, shard_index)}
    {
        m_connection->async_run(MakeRedisConfig(redis_config, shard_index, setup_commands),
                                boost::asio::consign(boost::asio::detached, m_connection));
    }

    template <typename Request, typename Response>
    boost::asio::awaitable<boost::system::error_code> AsyncExec(const Request& request, Response& response,
                                                                std::chrono::milliseconds timeout)
    {
        co_return co_await boost::asio::co_spawn(
            m_connection->get_executor(),
            [this, &request, &response, timeout]() -> boost::asio::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await m_connection->async_exec(request, response,
                                                  boost::asio::redirect_error(boost::asio::cancel_after(timeout), ec));
                co_return ec;
            },
            boost::asio::use_awaitable);
    }

    ~Shard()
    {
        if (m_connection)
        {
            m_connection->cancel();
        }
    }

private:
    boost::asio::strand<boost::asio::any_io_executor> m_strand;
    std::shared_ptr<boost::redis::connection> m_connection;
    std::size_t m_shard_index;
    std::string m_shard_master_name;
};

} // namespace run::redis