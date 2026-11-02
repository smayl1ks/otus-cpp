#include "network/server/server.hpp"
#include "network/server/server_bootstrap.hpp"

#include "envelope.pb.h"
#include "gamemap.pb.h"
#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "logger/logger.hpp"
#include "monitoring/metrics_collector.hpp"
#include "network/server/envelope_dispatcher.hpp"
#include "network/server/http_session.hpp"
#include "storage/redis/redis_pub_sub_worker.hpp"
#include "storage/redis/sentinel_config.hpp"

#include <algorithm>
#include <cmath>

namespace run::server
{

namespace
{

// A viewport bbox big enough for any city camera yet small enough that the
// res-9 cell expansion behind the legacy fallback can never explode
// (audit N1: unvalidated client bbox used to allocate unbounded vectors).
bool IsValidViewportBBox(const gamemap::SubscribeViewportRequest& request)
{
    constexpr double kMaxLatSpanDeg = 2.0;
    constexpr double kMaxLngSpanDeg = 4.0;

    const double sw_lat = request.south_west_lat();
    const double sw_lng = request.south_west_lng();
    const double ne_lat = request.north_east_lat();
    const double ne_lng = request.north_east_lng();

    return std::isfinite(sw_lat) && std::isfinite(sw_lng) && std::isfinite(ne_lat) && std::isfinite(ne_lng) &&
           std::abs(sw_lat) <= 90.0 && std::abs(ne_lat) <= 90.0 && std::abs(sw_lng) <= 180.0 &&
           std::abs(ne_lng) <= 180.0 && sw_lat <= ne_lat && sw_lng <= ne_lng && (ne_lat - sw_lat) <= kMaxLatSpanDeg &&
           (ne_lng - sw_lng) <= kMaxLngSpanDeg;
}

constexpr std::size_t kMaxSubscribedZones = 128;

std::vector<uint64_t> NormalizeZones(const google::protobuf::RepeatedField<uint64_t>& requested)
{
    std::vector<uint64_t> zones;
    zones.reserve(requested.size());
    for (const uint64_t zone : requested)
    {
        if (zone != 0)
        {
            zones.push_back(zone);
        }
    }
    std::sort(zones.begin(), zones.end());
    zones.erase(std::unique(zones.begin(), zones.end()), zones.end());
    if (zones.size() > kMaxSubscribedZones)
    {
        zones.resize(kMaxSubscribedZones);
    }
    return zones;
}

} // namespace

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

    try
    {
        auto [redis_cfg, buf_cfg] = storage::redis::LoadStorageConfig("runner.cfg");
        m_pubsub_worker = std::make_shared<storage::redis::RedisPubSubWorker>(
            m_context.GetLowerLayerIOContext().get_executor(), redis_cfg, nullptr, "kazan:zone:*");

        auto session_mgr = m_session_manager;
        auto h3_svc = m_running_services ? m_running_services->h3_service : nullptr;

        m_pubsub_worker->SetMessageHandler(
            [session_mgr, h3_svc](std::string_view channel, std::string_view payload)
            {
                run::proto::Envelope env;
                if (channel.find(":radar:") != std::string_view::npos)
                {
                    // Radar channels carry a bare RadarPing; wrap it for clients.
                    gamemap::RadarPing ping;
                    if (!ping.ParseFromArray(payload.data(), static_cast<int>(payload.size())))
                    {
                        return;
                    }

                    env.set_timestamp(ping.timestamp());
                    env.set_protocol_version(1);
                    *env.mutable_radar_ping() = ping;
                }
                else
                {
                    // Every other zone channel (capture events) carries a
                    // ready-to-send Envelope.
                    if (!env.ParseFromArray(payload.data(), static_cast<int>(payload.size())) ||
                        !env.has_hexagon_capture_event())
                    {
                        return;
                    }
                }

                std::string serialized;
                if (!env.SerializeToString(&serialized))
                {
                    return;
                }

                uint64_t hex9 = storage::redis::RedisPubSubWorker::ExtractH3Zone(channel);
                if (hex9 != 0 && h3_svc)
                {
                    if (auto parent6 = h3_svc->GetParent(hex9, 6); parent6.has_value())
                    {
                        session_mgr->BroadcastToZone(*parent6, serialized);
                    }
                }
            });

        m_pubsub_worker->Start();
        LOG_INFO("Redis Pub/Sub radar subscriber started on pattern 'kazan:zone:*'");
    }
    catch (const std::exception& e)
    {
        LOG_WARN("Could not initialize RedisPubSubWorker: {}", e.what());
    }

    if (server_config.IsSSLEnabled())
    {
        m_context.EnableSSL();
    }

    SetupProtobufRoutes();

    const auto& network = server_config.GetNetworkSettings();

    // Start Prometheus exposer on configured or default metrics port (9100)
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

    // Start HTTPS listener if SSL is enabled
    if (ssl_enabled)
    {
        net::co_spawn(m_context.GetLowerLayerIOContext(), ListenAsync(network.https_port, true), net::detached);
    }

    // Graceful shutdown
    m_context.SetShutdownHook(
        [this]()
        {
            if (m_pubsub_worker)
            {
                m_pubsub_worker->Stop();
            }
            monitoring::MetricsCollector::Instance().StopServer();
            m_session_manager->CloseAllSessions();
            net::co_spawn(
                m_context.GetLowerLayerIOContext(),
                [this]() -> net::awaitable<void>
                {
                    net::steady_timer drain_timer(m_context.GetLowerLayerIOContext(), std::chrono::seconds(3));
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
    // SubscribeViewport is fully handled here (zone subscription + optional
    // snapshot) and never reaches the dispatcher: the response carries the
    // snapshot, which the storage-free dispatcher cannot build.
    m_protobuf_controller->SetFallbackHandler(
        [services = m_running_services](const run::proto::Envelope& envelope,
                                        session::UserSession& session) -> net::awaitable<void>
        {
            if (envelope.payload_case() == run::proto::Envelope::PayloadCase::kSubscribeViewportRequest &&
                services->h3_service)
            {
                const auto& request = envelope.subscribe_viewport_request();

                // Zone-aware clients send their full res-6 set (idempotent
                // replace); legacy clients fall back to bbox expansion.
                std::vector<uint64_t> zones;
                if (!request.zone_h3().empty())
                {
                    zones = NormalizeZones(request.zone_h3());
                }
                else if (IsValidViewportBBox(request))
                {
                    const auto cells = services->h3_service->GetCellsInBoundingBox(
                        request.south_west_lat(), request.south_west_lng(), request.north_east_lat(),
                        request.north_east_lng(), 9);
                    zones = services->h3_service->GetParentZones(cells, 6);
                }

                session.UpdateSubscribedZones(zones);

                auto response = MakeResponseEnvelope(envelope);
                auto* viewport = response.mutable_subscribe_viewport_response();
                viewport->set_status(common::SYSTEM_STATUS_OK);

                if (request.need_snapshot() && services->hex_map_cache && !zones.empty())
                {
                    for (auto& hex : co_await services->hex_map_cache->GetSnapshotAsync(zones))
                    {
                        auto* message = viewport->add_hexagons();
                        message->set_h3_index(hex.h3_index);
                        message->set_owner_user_id(hex.owner_user_id);
                        message->set_owner_username(hex.owner_username);
                        message->set_owner_color_hex(hex.owner_color_hex);
                        message->set_score(hex.score);
                    }
                }

                session.SendProtobuf(response);
                co_return;
            }

            auto response =
                co_await run::server::DispatchAsync(static_cast<int64_t>(session.GetId()), envelope, *services);
            if (response.has_value())
            {
                session.SendProtobuf(*response);

                // Zombie-session reaping: a closed run ends the reason for
                // this connection. Without the server-side close, keepalive
                // pings plus the nginx 4h read timeout keep dead sockets
                // alive forever (audit §0.7); clients reconnect for map
                // browsing. The response above was already delivered.
                bool run_closed_ok = false;
                if (response->payload_case() == run::proto::Envelope::PayloadCase::kFinishRunResponse)
                {
                    run_closed_ok = response->finish_run_response().system_status() == common::SYSTEM_STATUS_OK;
                }
                else if (response->payload_case() == run::proto::Envelope::PayloadCase::kCancelRunResponse)
                {
                    run_closed_ok = response->cancel_run_response().system_status() == common::SYSTEM_STATUS_OK;
                }
                if (run_closed_ok)
                {
                    session.RequestClose();
                }
            }
        });
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
