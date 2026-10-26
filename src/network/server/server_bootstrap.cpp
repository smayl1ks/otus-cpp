#include "network/server/server_bootstrap.hpp"

#include <algorithm>
#include <thread>
#include <fmt/format.h>

#include "logger/logger.hpp"
#include "repository/connection_pool.hpp"
#include "repository/in_memory_auth_stores.hpp"
#include "repository/in_memory_stores.hpp"
#include "repository/postgres_run_history_repository.hpp"
#include "repository/postgres_user_repository.hpp"
#include "utils/crypto.hpp"

namespace run::server
{

bool ServerBootstrap::InitializePostgresPool(const config::PostgresConfig& config)
{
    try
    {
        const auto pool_size = config.pool_size > 0
            ? config.pool_size
            : std::max(1u, std::thread::hardware_concurrency() / 2);

        repository::ConnectionPool::Get().Init(config.ConnectionString(), pool_size);
        LOG_INFO("PostgreSQL connection pool initialized with {} connection(s) to {}:{}",
                 pool_size, config.host, config.port);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("PostgreSQL unreachable ({}), using in-memory persistence (dev mode)", e.what());
        return false;
    }
}

void ServerBootstrap::SeedDevUserIfConfigured(repository::IUserRepository& repo,
                                            const config::DevConfig& config)
{
    if (!config.HasDevUser())
    {
        LOG_WARN("No dev user configured (RUN_DEV_USERNAME / RUN_DEV_PASSWORD): login will reject everyone");
        return;
    }

    auto* in_memory_users = dynamic_cast<repository::InMemoryUserRepository*>(&repo);
    if (!in_memory_users)
    {
        return;
    }

    repository::User user;
    user.id = 1;
    user.username = config.username;
    user.email = fmt::format("{}@dev.local", config.username);
    user.password_hash = utils::crypto::HashPassword(config.password);
    in_memory_users->PutUserForTest(user);
    LOG_INFO("Dev user '{}' seeded", config.username);
}

ServerDependencies ServerBootstrap::BuildDependencies(context::WorkContext& work_context,
                                                     const config::ServerConfig& config)
{
    ServerDependencies deps;

    bool pg_ready = InitializePostgresPool(config.GetPostgresSettings());

    if (pg_ready)
    {
        deps.user_repository = std::make_shared<repository::PostgresUserRepository>(work_context);
        deps.run_history_repository = std::make_shared<repository::PostgresRunHistoryRepository>(work_context);
    }
    else
    {
        deps.user_repository = std::make_shared<repository::InMemoryUserRepository>();
        SeedDevUserIfConfigured(*deps.user_repository, config.GetDevSettings());
        deps.run_history_repository = std::make_shared<repository::InMemoryRunHistoryRepository>();
    }

    deps.redis_repository = std::make_shared<repository::InMemoryRedisRepository>();

    deps.auth_service = std::make_shared<service::AuthService>(
        deps.user_repository, deps.redis_repository, work_context);

    deps.http_controller = std::make_shared<controller::HttpController>(
        deps.auth_service, deps.user_repository);

    deps.protobuf_controller = std::make_shared<controller::ProtobufController>();

    auto service_deps = service::running::MakeStubDeps();
    service_deps.history_repository = deps.run_history_repository;
    deps.service_stack = std::make_shared<service::running::ServiceStack>(std::move(service_deps));

    return deps;
}

} // namespace run::server
