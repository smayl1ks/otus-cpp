#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "common.hpp"
#include "logger/logger.hpp"

#include "network/server/session_manager.hpp"
#include "network/server/user_session.hpp"

#include <boost/asio/experimental/concurrent_channel.hpp>

#include "controllers/protobuf_controller.hpp"

#include "envelope.pb.h"

namespace run::server
{

template <typename Stream>
class WebSocketSession : public session::UserSession, public std::enable_shared_from_this<WebSocketSession<Stream>>
{
private:
    // Thread-safe channel: Send() may be called from any executor without a spawn.
    using WriteChannel = net::experimental::concurrent_channel<void(sys::error_code, std::string)>;

    static constexpr std::size_t kMaxFrameBytes = 256 * 1024; // largest legal Envelope ~60 KB
    static constexpr std::size_t kWriteQueueLimit = 128;
    static constexpr int kMaxMalformedFrames = 10; // garbage flood -> disconnect

    websocket::stream<Stream> m_ws;
    beast::flat_buffer m_buffer;

    WriteChannel m_write_channel;

    std::shared_ptr<controller::ProtobufController> m_controller;
    std::shared_ptr<session::SessionManager> m_session_manager;

    uint64_t m_user_id{0};
    uint64_t m_active_run_id{0};
    std::unordered_set<uint64_t> m_subscribed_zones;
    std::atomic<bool> m_closed{false};

private:
    net::awaitable<void> ReadLoop()
    {
        int malformed_frames = 0;

        try
        {
            for (;;)
            {
                m_buffer.clear();
                m_buffer.max_size(kMaxFrameBytes);
                co_await m_ws.async_read(m_buffer, net::use_awaitable);

                run::proto::Envelope envelope;
                auto bytes = m_buffer.data();
                if (!envelope.ParseFromArray(bytes.data(), bytes.size()))
                {
                    LOG_DEBUG("Failed to parse Protobuf payload from user {}", m_user_id);
                    if (++malformed_frames >= kMaxMalformedFrames)
                    {
                        LOG_WARN("User {} sent too many malformed frames, disconnecting", m_user_id);
                        RequestClose();
                        break;
                    }
                    continue;
                }

                malformed_frames = 0;
                co_await m_controller->Dispatch(envelope, *this);
            }
        }
        catch (const std::exception& ex)
        {
            LOG_DEBUG("WebSocket read loop finished for user {}: {}", m_user_id, ex.what());
        }

        // Reader is gone: no more writes make sense.
        m_write_channel.close();
    }

    net::awaitable<void> WriteLoop()
    {
        for (;;)
        {
            auto [ec, msg] = co_await m_write_channel.async_receive(net::as_tuple(net::use_awaitable));
            if (ec)
            {
                break;
            }

            if (msg.empty())
            {
                // Close sentinel from RequestClose(): everything queued before
                // it has been written, so the close handshake is race-free.
                sys::error_code close_ec;
                co_await m_ws.async_close(websocket::close_code::going_away,
                                          net::redirect_error(net::use_awaitable, close_ec));
                break;
            }

            sys::error_code write_ec;
            co_await m_ws.async_write(net::buffer(msg), net::redirect_error(net::use_awaitable, write_ec));

            if (write_ec)
            {
                LOG_DEBUG("Write error for user {}: {}", m_user_id, write_ec.message());
                // Without this the read loop would wait forever on a dead socket.
                Close();
                break;
            }
        }
    }

public:
    explicit WebSocketSession(Stream&& stream, std::shared_ptr<controller::ProtobufController> controller,
                              std::shared_ptr<session::SessionManager> session_manager, uint64_t user_id = 0)
        : m_ws(std::move(stream)), m_write_channel(m_ws.get_executor(), kWriteQueueLimit),
          m_controller(std::move(controller)), m_session_manager(std::move(session_manager)), m_user_id(user_id)
    {
        m_buffer.max_size(kMaxFrameBytes);
    }

    ~WebSocketSession() override
    {
        if (m_session_manager)
        {
            m_session_manager->RemoveSession(m_user_id, this);
        }
    }

    /// Graceful: queued payloads are flushed first, then 1001 Going Away.
    /// Empty string is the close sentinel (legal envelopes are never empty).
    void RequestClose() override
    {
        bool expected = false;
        if (!m_closed.compare_exchange_strong(expected, true))
        {
            return;
        }
        if (!m_write_channel.try_send(sys::error_code{}, std::string{}))
        {
            Close(); // queue full or loop already gone
        }
    }

    /// Hard cut: socket dies immediately, both loops exit with errors.
    void Close() override
    {
        m_closed = true;
        m_write_channel.close();
        beast::error_code ec;
        beast::get_lowest_layer(m_ws).socket().close(ec);
    }

    template <typename Body, typename Allocator>
    net::awaitable<void> Run(http::request<Body, http::basic_fields<Allocator>> request)
    {
        auto self = this->shared_from_this();

        beast::get_lowest_layer(m_ws).expires_never();

        m_ws.binary(true);
        // Idle timeout also reaps connections whose peer vanished without
        // FIN/RST; pings keep alive long-idle but healthy clients (NAT).
        websocket::stream_base::timeout opt{
            std::chrono::seconds(30), // handshake timeout
            std::chrono::minutes(5),  // idle timeout: no read/write activity for 5 min -> close
            true                      // keep-alive pings during idle periods
        };
        m_ws.set_option(opt);

        sys::error_code ec;
        co_await m_ws.async_accept(request, net::redirect_error(net::use_awaitable, ec));

        if (ec)
        {
            LOG_DEBUG("WebSocket accept failed for user {}: {}", m_user_id, ec.message());
            co_return;
        }

        if (m_session_manager)
        {
            m_session_manager->AddSession(m_user_id, self);
        }

        LOG_DEBUG("User {} connected via WebSocket", m_user_id);

        auto executor = co_await net::this_coro::executor;

        net::co_spawn(executor, [self]() { return self->WriteLoop(); }, net::detached);

        co_await ReadLoop();
    }

    net::any_io_executor GetExecutor() override
    {
        return m_ws.get_executor();
    }

    uint64_t GetId() const noexcept override
    {
        return m_user_id;
    }

    void Send(std::string message) override
    {
        if (m_closed)
        {
            return;
        }
        if (!m_write_channel.try_send(sys::error_code{}, std::move(message)))
        {
            LOG_WARN("Write channel overflow for user {}, dropping packet", m_user_id);
        }
    }

    void SendProtobuf(const run::proto::Envelope& envelope) override
    {
        std::string payload;
        if (!envelope.SerializeToString(&payload))
        {
            LOG_ERROR("Failed to serialize Protobuf Envelope for user {}", m_user_id);
            return;
        }
        Send(std::move(payload));
    }

    void UpdateSubscribedZones(const std::vector<uint64_t>& zones) override
    {
        m_subscribed_zones.clear();
        m_subscribed_zones.insert(zones.begin(), zones.end());

        if (m_session_manager)
        {
            m_session_manager->UpdateViewportSubscription(this->shared_from_this(), zones);
        }
    }

    const std::unordered_set<uint64_t>& GetSubscribedZones() const noexcept override
    {
        return m_subscribed_zones;
    }

    uint64_t GetActiveRunId() const noexcept override
    {
        return m_active_run_id;
    }

    void SetActiveRunId(uint64_t run_id) noexcept override
    {
        m_active_run_id = run_id;
    }
};

} // namespace run::server
