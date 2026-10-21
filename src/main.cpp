#include <boost/asio.hpp>
#include <boost/redis/config.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/logger.hpp>
#include <string>

#include "logger/logger.hpp"

struct RedisUser
{
    std::string name;
    std::string password;
};

boost::asio::awaitable<void> co_main(std::shared_ptr<boost::redis::connection> redis_connection, const RedisUser& user)
{
    boost::redis::config master_config;
    master_config.use_ssl = false;
    master_config.addr = boost::redis::address{"127.0.0.1", "6379"};
    master_config.username = user.name;
    master_config.password = user.password;

    redis_connection->async_run(master_config, boost::asio::consign(boost::asio::detached, redis_connection));

    co_return;
}

int main()
{
    boost::asio::io_context io_context;

    // clang-format off
    boost::redis::logger redis_logger{boost::redis::logger::level::info,
        [](boost::redis::logger::level level, std::string_view message)
            {
                switch (level)
                {
                case boost::redis::logger::level::debug:
                    LOG_DEBUG("[Boost-Redis] {}", message);
                    break;
                case boost::redis::logger::level::err:
                    LOG_ERROR("[Boost-Redis] {}", message);
                    break;
                default:
                    LOG_INFO("[Boost-Redis] {}", message);
                    break;
                }
            }};
    // clang-format on
    auto redis_connection = std::make_shared<boost::redis::connection>(io_context, redis_logger);
    boost::asio::co_spawn(io_context, co_main(redis_connection, {"mymaster", "12345"}), boost::asio::detached);
    io_context.run();
    return 0;
}