#include "network/server/server.hpp"

#include "network/server/envelope_dispatcher.hpp"
#include "repository/connection_pool.hpp"
#include "repository/postgres_run_history_repository.hpp"
#include "repository/postgres_user_repository.hpp"
#include "utils/crypto.hpp"

#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>
#include <thread>

namespace run::server
{

namespace
{

std::string EnvOr(const char* name, const std::string& fallback)
{
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0')
    {
        return value;
    }
    return fallback;
}

std::string BuildConnectionString()
{
    // Names follow docker-compose.yml / .env.
    return fmt::format("host={} port={} dbname={} user={} password={}", EnvOr("POSTGRES_HOST", "localhost"),
                       EnvOr("POSTGRES_PORT", "5432"), EnvOr("POSTGRES_DB", "run_uram_db"),
                       EnvOr("POSTGRES_USER", "postgres_user"), EnvOr("POSTGRES_PASSWORD", ""));
}

/// Connects the pool once; false = PostgreSQL unreachable (dev fallback).
bool EnsurePoolInitialized()
{
    static const bool initialized = []()
    {
        try
        {
            const auto pool_size = std::max(1u, std::thread::hardware_concurrency() / 2);
            run::repository::ConnectionPool::Get().Init(BuildConnectionString(), pool_size);
            return true;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("PostgreSQL unreachable ({}), using in-memory persistence (dev mode)", e.what());
            return false;
        }
    }();
    return initialized;
}

std::shared_ptr<run::repository::IUserRepository> CreateUserRepository(run::context::WorkContext& work)
{
    if (EnsurePoolInitialized())
    {
        return std::make_shared<run::repository::PostgresUserRepository>(work);
    }
    return std::make_shared<run::repository::InMemoryUserRepository>();
}

std::shared_ptr<run::repository::IRunHistoryRepository> CreateRunHistoryRepository(run::context::WorkContext& work)
{
    if (EnsurePoolInitialized())
    {
        return std::make_shared<run::repository::PostgresRunHistoryRepository>(work);
    }
    return std::make_shared<run::repository::InMemoryRunHistoryRepository>();
}

run::service::running::ServiceDeps BuildServiceDeps(std::shared_ptr<run::repository::IRunHistoryRepository> history)
{
    auto deps = run::service::running::MakeStubDeps();
    if (history != nullptr)
    {
        deps.history_repository = std::move(history);
    }
    return deps;
}

// Dev bootstrap until PostgreSQL lands: one seeded user from env makes the
// whole chain (login -> ws-ticket -> WS -> run pipeline) testable end to end.
void SeedDevUser(run::repository::InMemoryUserRepository& users)
{
    const char* username = std::getenv("RUN_DEV_USERNAME");
    const char* password = std::getenv("RUN_DEV_PASSWORD");
    if (username == nullptr || password == nullptr)
    {
        LOG_WARN("No dev user configured (RUN_DEV_USERNAME / RUN_DEV_PASSWORD): login will reject everyone");
        return;
    }

    run::repository::User user;
    user.id = 1;
    user.username = username;
    user.email = fmt::format("{}@dev.local", username);
    user.password_hash = run::utils::crypto::HashPassword(password);
    users.PutUserForTest(user);
    LOG_INFO("Dev user '{}' seeded", username);
}

} // namespace

Server::Server(unsigned int io_threads, unsigned int work_threads)
    : m_context(io_threads, work_threads), m_session_manager(std::make_shared<session::SessionManager>()),
      m_user_repository(CreateUserRepository(m_context.GetWorkContext())),
      m_redis_repository(std::make_shared<run::repository::InMemoryRedisRepository>()),
      m_run_history(CreateRunHistoryRepository(m_context.GetWorkContext())),
      m_auth_service(std::make_shared<run::service::AuthService>(m_user_repository, m_redis_repository,
                                                                 m_context.GetWorkContext())),
      m_http_controller(std::make_shared<controller::HttpController>(m_auth_service, m_user_repository)),
      m_protobuf_controller(std::make_shared<controller::ProtobufController>()),
      m_service_stack(std::make_shared<run::service::running::ServiceStack>(
          BuildServiceDeps(m_run_history)))
{
    const auto& server_config = config::ServerConfig::Get();

    LOG_INFO("--- {} ---", server_config.GetServerName());
    LOG_INFO("Version: {}", server_config.GetServerVersion());

    // Dev seed only applies to the in-memory store; PostgreSQL users come from the DB.
    if (auto* memory_users = dynamic_cast<run::repository::InMemoryUserRepository*>(m_user_repository.get());
        memory_users != nullptr)
    {
        SeedDevUser(*memory_users);
    }

    if (server_config.IsSSLEnabled())
    {
        m_context.EnableSSL();
    }

    SetupProtobufRoutes();

    const auto& network = server_config.GetNetworkSettings();
    const bool use_tls = m_context.GetSSLContext() != nullptr;
    net::co_spawn(m_context.GetLowerLayerIOContext(),
                  ListenAsync(use_tls ? network.https_port : network.http_port, use_tls), net::detached);

    // Graceful shutdown: 1001 Going Away to every session, short drain, stop.
    m_context.SetShutdownHook(
        [this]()
        {
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

        // One strand per connection: session callbacks serialize on it.
        net::co_spawn(
            net::make_strand(executor),
            [socket = std::move(socket), http_controller = m_http_controller,
             protobuf_controller = m_protobuf_controller, session_manager = m_session_manager,
             auth_service = m_auth_service]() mutable -> net::awaitable<void>
            {
                // TLS terminates on nginx in production; the ssl_stream
                // instantiation of HttpSession is compile-checked in tests.
                auto session = std::make_shared<HttpSession<beast::tcp_stream>>(
                    beast::tcp_stream(std::move(socket)), std::move(http_controller), std::move(protobuf_controller),
                    std::move(session_manager), std::move(auth_service));

                co_await session->Run();
            },
            net::detached);
    }
}

void Server::SetupProtobufRoutes()
{
    m_protobuf_controller->SetFallbackHandler(
        [stack = m_service_stack](const run::proto::Envelope& envelope,
                                  session::UserSession& session) -> net::awaitable<void>
        {
            auto response =
                co_await run::network::server::DispatchAsync(static_cast<int64_t>(session.GetId()), envelope, *stack);
            if (response.has_value())
            {
                session.SendProtobuf(*response);
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
