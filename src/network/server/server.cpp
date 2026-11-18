#include "network/server/server.hpp"
#include "network/server/server_bootstrap.hpp"

#include "controllers/protobuf_routes.hpp"
#include "logger/logger.hpp"
#include "monitoring/metrics_collector.hpp"
#include "network/server/http_session.hpp"
#include "network/server/radar_fanout.hpp"

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <sys/socket.h>
#endif

namespace run::server
{

Server::Server(unsigned int io_threads, unsigned int work_threads)
    : m_context(io_threads, work_threads), m_session_manager(std::make_shared<session::SessionManager>())
{
    const auto& server_config = config::ServerConfig::Get();

    LOG_INFO("--- {} ---", server_config.GetServerName());
    LOG_INFO("Version: {}", server_config.GetServerVersion());

    auto deps = ServerBootstrap::BuildDependencies(m_context.GetWorkContext(),
                                                   m_context.GetLowerLayerIOContext().get_executor(), server_config);
    m_user_repository = std::move(deps.user_repository);
    m_redis_repository = std::move(deps.redis_repository);
    m_run_history = std::move(deps.run_history_repository);
    m_auth_service = std::move(deps.auth_service);
    m_http_controller = std::move(deps.http_controller);
    m_protobuf_controller = std::move(deps.protobuf_controller);
    m_running_services = std::move(deps.running_services);

    // Map cache warm-up
    if (m_running_services && m_running_services->hex_map_cache)
    {
        net::co_spawn(
            m_context.GetLowerLayerIOContext(), [cache = m_running_services->hex_map_cache]() -> net::awaitable<void>
            { (void)co_await cache->WarmUpAsync(); }, net::detached);
    }

    try
    {
        run::redis::FleetSubscription subscription;
        subscription.patterns = {"kazan:zone:*"};
        subscription.on_message =
            RadarFanout{m_session_manager, m_running_services ? m_running_services->h3_service : nullptr};

        boost::asio::any_io_executor io = m_context.GetLowerLayerIOContext().get_executor();
        m_radar_subscriber =
            std::make_unique<run::redis::FleetSubscriber>(io, deps.redis_sentinel_config, std::move(subscription));
        LOG_INFO("Redis fleet subscriber started on pattern 'kazan:zone:*'");
    }
    catch (const std::exception& e)
    {
        LOG_WARN("Could not start the radar fleet subscriber: {}", e.what());
    }

    if (server_config.IsSSLEnabled())
    {
        m_context.EnableSSL();
    }

    SetupProtobufRoutes();

    const auto& network = server_config.GetNetworkSettings();

    const std::string metrics_address = fmt::format("{}:9100", network.address);
    if (!monitoring::MetricsCollector::Instance().StartServer(metrics_address))
    {
        LOG_WARN("Could not bind Prometheus exposer to {} (port may already be in use)", metrics_address);
    }
    else
    {
        LOG_INFO("Prometheus metrics exposer listening on http://{}/metrics", metrics_address);
    }
    const bool ssl_enabled = m_context.GetSSLContext() != nullptr;

    net::co_spawn(m_context.GetLowerLayerIOContext(), ListenAsync(network.http_port, false), net::detached);

    if (ssl_enabled)
    {
        net::co_spawn(m_context.GetLowerLayerIOContext(), ListenAsync(network.https_port, true), net::detached);
    }

    // Graceful shutdown
    m_context.SetShutdownHook(
        [this]()
        {
            m_radar_subscriber.reset();
            monitoring::MetricsCollector::Instance().StopServer();
            m_session_manager->CloseAllSessions();
            net::co_spawn(
                m_context.GetLowerLayerIOContext(),
                [this, drain_seconds = config::ServerConfig::Get().GetNetworkSettings().shutdown_drain_seconds]() -> net::awaitable<void>
                {
                    net::steady_timer drain_timer(m_context.GetLowerLayerIOContext(),
                                                  std::chrono::seconds(drain_seconds));
                    co_await drain_timer.async_wait(net::use_awaitable);
                    Stop();
                },
                net::detached);
        });
}

net::awaitable<void> Server::ListenAsync(unsigned short port, bool use_tls)
{
    auto executor = co_await net::this_coro::executor;
    const auto& address_str = config::ServerConfig::Get().GetNetworkSettings().address;
    tcp::endpoint endpoint(net::ip::make_address(address_str), port);

    tcp::acceptor acceptor(executor);
    sys::error_code ec;

    acceptor.open(endpoint.protocol(), ec);
    if (ec)
    {
        LOG_ERROR("Acceptor open failed on port {}: {}", port, ec.message());
        co_return;
    }

    acceptor.set_option(net::socket_base::reuse_address(true), ec);

    // Allows several server instances (one acceptor per instance) to bind the
    // same port and let the kernel distribute incoming connections between
    // them; Linux 3.9+, other platforms run a single acceptor (not fatal)
#if defined(__linux__)
    {
        const int reuse_port_enabled = 1;
        if (::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &reuse_port_enabled,
                         sizeof(reuse_port_enabled)) != 0)
        {
            LOG_WARN("SO_REUSEPORT is not available on port {}: {}", port, std::strerror(errno));
        }
    }
#endif

    acceptor.bind(endpoint, ec);
    if (ec)
    {
        LOG_ERROR("Acceptor bind failed on port {}: {}", port, ec.message());
        co_return;
    }

    acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec)
    {
        LOG_ERROR("Acceptor listen failed on port {}: {}", port, ec.message());
        co_return;
    }

    LOG_INFO("{} server listening on {}:{}", use_tls ? "HTTPS" : "HTTP", address_str, port);

    for (;;)
    {
        tcp::socket socket = co_await acceptor.async_accept(net::redirect_error(net::use_awaitable, ec));
        if (ec)
        {
            LOG_WARN("Accept error: {}", ec.message());
            net::steady_timer backoff(executor, std::chrono::milliseconds(100));
            co_await backoff.async_wait(net::use_awaitable);
            continue;
        }

        monitoring::MetricsCollector::Instance().IncrementConnectionsAccepted(use_tls);

        // Radar pings and telemetry acks are small frames: without NODELAY
        // Nagle x delayed-ACK interaction can stall them for up to tens of ms
        sys::error_code no_delay_ec;
        socket.set_option(net::ip::tcp::no_delay(true), no_delay_ec);
        if (no_delay_ec)
        {
            LOG_WARN("Failed to set TCP_NODELAY on a new connection: {}", no_delay_ec.message());
        }

        if (use_tls)
        {
            auto* ssl_ctx = m_context.GetSSLContext();
            if (!ssl_ctx)
            {
                LOG_ERROR("TLS socket received, but SSLContext is null");
                continue;
            }

            net::co_spawn(
                net::make_strand(executor),
                [socket = std::move(socket), &ssl_context = ssl_ctx->GetSSLContext(),
                 http_controller = m_http_controller, protobuf_controller = m_protobuf_controller,
                 session_manager = m_session_manager, auth_service = m_auth_service]() mutable -> net::awaitable<void>
                {
                    beast::ssl_stream<beast::tcp_stream> stream(std::move(socket), ssl_context);
                    auto session = std::make_shared<HttpSession<beast::ssl_stream<beast::tcp_stream>>>(
                        std::move(stream), std::move(http_controller), std::move(protobuf_controller),
                        std::move(session_manager), std::move(auth_service));
                    co_await session->Run();
                },
                net::detached);
        }
        else
        {
            net::co_spawn(
                net::make_strand(executor),
                [socket = std::move(socket), http_controller = m_http_controller,
                 protobuf_controller = m_protobuf_controller, session_manager = m_session_manager,
                 auth_service = m_auth_service]() mutable -> net::awaitable<void>
                {
                    auto session = std::make_shared<HttpSession<beast::tcp_stream>>(
                        beast::tcp_stream(std::move(socket)), std::move(http_controller),
                        std::move(protobuf_controller), std::move(session_manager), std::move(auth_service));
                    co_await session->Run();
                },
                net::detached);
        }
    }
}

void Server::SetupProtobufRoutes()
{
    controller::routes::SetupProtobufRoutes(*m_protobuf_controller, m_running_services);
}

void Server::Run()
{
    m_context.Run();
}

void Server::Stop()
{
    m_context.Stop();
}

} // namespace run::server
