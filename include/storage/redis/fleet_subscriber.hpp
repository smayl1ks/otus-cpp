#pragma once

#include "logger/logger.hpp"
#include "storage/redis/redis_config.hpp"
#include "storage/redis/sentinel_config.hpp"

#include <boost/asio.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/resp3/type.hpp>
#include <boost/redis/response.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace run::redis
{

using Node = boost::redis::resp3::node;
using Generic = boost::redis::generic_response;

/// Subscription configuration: patterns and message callback
struct FleetSubscription
{
    /// PSUBSCRIBE patterns (auto-restored by boost-redis on reconnect)
    std::vector<std::string> patterns;

    /// Called per message on the shard's strand (non-blocking, non-throwing).
    /// Views point to a temporary buffer; copy data if retained
    std::function<void(std::string_view channel, std::string_view payload)> on_message;
};

/// Dedicated Pub/Sub receiver across a shard fleet.
/// Fire-and-forget semantics with automatic reconnection and subscription recovery
class FleetSubscriber
{
public:
    FleetSubscriber(boost::asio::any_io_executor& io_context, const SentinelConfig& config, FleetSubscription spec)
    {
        if (!spec.on_message)
        {
            throw std::invalid_argument("FleetSubscription::on_message is required");
        }

        m_receivers.reserve(config.num_shards);
        for (std::size_t i = 0; i < config.num_shards; ++i)
        {
            m_receivers.push_back(std::make_shared<Receiver>(io_context, config, i, spec));
            boost::asio::co_spawn(m_receivers.back()->connection->get_executor(), ReceiveLoop(m_receivers.back()),
                                  [i](std::exception_ptr ep)
                                  {
                                      if (ep)
                                      {
                                          LOG_ERROR("FleetSubscriber: shard {} receive loop terminated", i);
                                      }
                                  });
        }
    }

    ~FleetSubscriber()
    {
        for (auto& receiver : m_receivers)
        {
            receiver->active = false;
            receiver->connection->cancel();
        }
    }

    FleetSubscriber(const FleetSubscriber&) = delete;
    FleetSubscriber& operator=(const FleetSubscriber&) = delete;

private:
    struct Receiver
    {
        Receiver(boost::asio::any_io_executor& io_context, const SentinelConfig& config, std::size_t index,
                 const FleetSubscription& subscription)
            : strand{boost::asio::make_strand(io_context)},
              connection{std::make_shared<boost::redis::connection>(strand)}, shard{index}, spec{subscription}
        {
            connection->async_run(MakeRedisConfig(config, index),
                                  boost::asio::consign(boost::asio::detached, connection));
        }

        boost::asio::strand<boost::asio::any_io_executor> strand;
        std::shared_ptr<boost::redis::connection> connection;
        std::size_t shard;
        FleetSubscription spec;
        std::atomic<bool> active{true};
    };

    static boost::asio::awaitable<void> ReceiveLoop(std::shared_ptr<Receiver> receiver)
    {
        auto& connection = *receiver->connection;

        // Send psubscribe (the request must outlive async_exec)
        boost::redis::request subscribe_req;
        subscribe_req.psubscribe(receiver->spec.patterns);
        boost::system::error_code ec;
        co_await connection.async_exec(subscribe_req, boost::redis::ignore,
                                       boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (!receiver->active || ec)
        {
            LOG_WARN("FleetSubscriber: shard {} psubscribe failed: {}", receiver->shard, ec.message());
            co_return;
        }

        Generic pushes;
        connection.set_receive_response(pushes);

        boost::asio::steady_timer backoff{receiver->strand};

        // Message reception loop with auto-reconnection and error handling
        while (receiver->active)
        {
            ec.clear();
            co_await connection.async_receive2(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (!receiver->active || ec == boost::asio::error::operation_aborted)
            {
                co_return;
            }
            if (ec)
            {
                LOG_WARN("FleetSubscriber: shard {} receive error: {}", receiver->shard, ec.message());
                if (!connection.will_reconnect())
                {
                    co_return;
                }
                backoff.expires_after(std::chrono::milliseconds{100});
                co_await backoff.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                continue;
            }

            Dispatch(receiver->spec, pushes, *receiver);
            pushes.value().clear();
        }
    }

    static void Dispatch(const FleetSubscription& spec, const Generic& pushes, const Receiver& receiver)
    {
        if (!pushes.has_value())
        {
            return;
        }

        const auto& nodes = *pushes;
        std::size_t pos = 0;
        while (pos < nodes.size())
        {
            const Node& head = nodes[pos];
            const std::size_t span = 1 + head.aggregate_size;

            if (head.data_type == boost::redis::resp3::type::push && head.aggregate_size >= 3)
            {
                const std::string_view kind = nodes[pos + 1].value;
                try
                {
                    // RESP3 push shapes: [message, channel, payload],
                    // [pmessage, pattern, channel, payload]
                    if (kind == "pmessage" && head.aggregate_size >= 4)
                    {
                        spec.on_message(nodes[pos + 3].value, nodes[pos + 4].value);
                    }
                    else if (kind == "message" && head.aggregate_size >= 3)
                    {
                        spec.on_message(nodes[pos + 2].value, nodes[pos + 3].value);
                    }
                }
                catch (const std::exception& e)
                {
                    LOG_ERROR("FleetSubscriber: shard {} handler failed: {}", receiver.shard, e.what());
                }
            }

            pos += span;
        }
    }

    std::vector<std::shared_ptr<Receiver>> m_receivers;
};

} // namespace run::redis
