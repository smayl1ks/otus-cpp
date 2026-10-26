#pragma once

#include <memory>
#include <type_traits>

#include "common.hpp"
#include "logger/logger.hpp"

#include "network/server/session_manager.hpp"
#include "network/server/websocket_session.hpp"

#include "controllers/http_controller.hpp"
#include "controllers/protobuf_controller.hpp"
#include "service/auth_service.hpp"

namespace run::server
{

template <typename Stream>
class HttpSession : public std::enable_shared_from_this<HttpSession<Stream>>
{
private:
    Stream m_stream;
    beast::flat_buffer m_buffer;
    http::request<http::string_body> m_request;

    std::shared_ptr<controller::HttpController> m_http_controller;
    std::shared_ptr<controller::ProtobufController> m_protobuf_controller;
    std::shared_ptr<session::SessionManager> m_session_manager;
    std::shared_ptr<service::AuthService> m_auth_service;

private:
    net::awaitable<void> HandleHttpRequest(http::request<http::string_body> request)
    {
        sys::error_code ec;
        auto response = co_await m_http_controller->HandleRequest(std::move(request));

        co_await http::async_write(m_stream, response, net::redirect_error(net::use_awaitable, ec));
        if (ec)
        {
            LOG_DEBUG("Failed to write HTTP response: {}", ec.message());
        }
    }

    std::string ExtractTokenFromTarget(std::string_view target)
    {
        auto query_pos = target.find('?');

        if (query_pos == std::string_view::npos)
        {
            return "";
        }

        std::string_view query = target.substr(query_pos + 1);
        size_t pos = query.find("token=");

        while (pos != std::string_view::npos)
        {
            if (pos == 0 || query[pos - 1] == '&')
            {
                size_t start = pos + 6; // "token="
                size_t end = query.find('&', start);
                if (end == std::string_view::npos)
                {
                    return std::string(query.substr(start));
                }

                return std::string(query.substr(start, end - start));
            }

            pos = query.find("token=", pos + 6);
        }

        return "";
    }

public:
    explicit HttpSession(Stream&& stream, std::shared_ptr<controller::HttpController> http_controller,
                         std::shared_ptr<controller::ProtobufController> protobuf_controller,
                         std::shared_ptr<session::SessionManager> session_manager,
                         std::shared_ptr<service::AuthService> auth_service)
        : m_stream(std::move(stream)), m_http_controller(std::move(http_controller)),
          m_protobuf_controller(std::move(protobuf_controller)), m_session_manager(std::move(session_manager)),
          m_auth_service(std::move(auth_service))
    {
    }

    net::awaitable<void> Run()
    {
        sys::error_code ec;

        if constexpr (std::is_same_v<Stream, beast::ssl_stream<beast::tcp_stream>>)
        {
            beast::get_lowest_layer(m_stream).expires_after(std::chrono::seconds(15));
            co_await m_stream.async_handshake(ssl::stream_base::server, net::redirect_error(net::use_awaitable, ec));
            if (ec)
            {
                LOG_DEBUG("TLS Handshake failed: {}", ec.message());
                co_return;
            }
        }

        for (;;)
        {
            m_request = {};
            beast::get_lowest_layer(m_stream).expires_after(std::chrono::seconds(15));
            co_await http::async_read(m_stream, m_buffer, m_request, net::redirect_error(net::use_awaitable, ec));

            if (ec == http::error::end_of_stream)
            {
                break;
            }
            if (ec)
            {
                LOG_DEBUG("HTTP read error: {}", ec.message());
                co_return;
            }

            if (beast::websocket::is_upgrade(m_request))
            {
                std::string token = ExtractTokenFromTarget(m_request.target());

                auto user_id_opt = co_await m_auth_service->ValidateTempToken(token);

                if (!user_id_opt.has_value() || *user_id_opt == 0)
                {
                    http::response<http::string_body> res{http::status::unauthorized, m_request.version()};
                    res.set(http::field::server, "Run Uram Server");
                    res.set(http::field::content_type, "application/json");
                    res.body() = R"({"status":"error","message":"Invalid or missing WebSocket token"})";
                    res.prepare_payload();

                    co_await http::async_write(m_stream, res, net::redirect_error(net::use_awaitable, ec));
                    co_return;
                }

                auto websocket_session = std::make_shared<WebSocketSession<Stream>>(
                    std::move(m_stream), m_protobuf_controller, m_session_manager, *user_id_opt);

                co_await websocket_session->Run(std::move(m_request));
                co_return;
            }
            else
            {
                bool keep_alive = m_request.keep_alive();
                co_await HandleHttpRequest(std::move(m_request));
                if (!keep_alive)
                {
                    break;
                }
            }
        }

        if constexpr (std::is_same_v<Stream, beast::tcp_stream>)
        {
            beast::error_code shutdown_ec;
            beast::get_lowest_layer(m_stream).socket().shutdown(tcp::socket::shutdown_send, shutdown_ec);
        }
        else
        {
            beast::error_code shutdown_ec;
            beast::get_lowest_layer(m_stream).expires_after(std::chrono::seconds(5));
            co_await m_stream.async_shutdown(net::redirect_error(net::use_awaitable, shutdown_ec));
        }
    }
};

} // namespace run::server