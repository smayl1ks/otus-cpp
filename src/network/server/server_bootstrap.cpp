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
#include "storage/redis/redis_active_run_store.hpp"
#include "storage/redis/redis_pub_sub_publisher.hpp"
#include "storage/redis/redis_repository.hpp"
#include "storage/redis/sentinel_config.hpp"
#include "storage/redis/sharded_redis_manager.hpp"
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

    const char* force_in_mem = std::getenv("RUN_USE_IN_MEMORY_STORES");
    const bool use_in_memory = (force_in_mem != nullptr && std::string(force_in_mem) == "1");

    service::running::ServiceDeps service_deps;
    service_deps.history_repository = deps.run_history_repository;

    if (!use_in_memory)
    {
        auto [redis_cfg, buf_cfg] = storage::redis::LoadStorageConfig("runner.cfg");
        auto redis_manager = std::make_shared<storage::redis::ShardedRedisManager>(
            work_context.GetExecutor(), std::move(redis_cfg), buf_cfg);

        // ResilientRedisProducer handles Sentinel failover and circuit breaker buffering
        boost::asio::co_spawn(work_context.GetExecutor(),
                              redis_manager->InitializeAsync(), boost::asio::detached);

        deps.redis_repository = std::make_shared<repository::RedisRepository>(redis_manager);
        service_deps.active_run_store = std::make_shared<storage::redis::RedisActiveRunStore>(redis_manager);
        service_deps.radar_transport = std::make_shared<storage::redis::RedisPubSubPublisher>(redis_manager);
        LOG_INFO("Redis storage plane active (stateless node with circuit-breaker buffer)");
    }
    else
    {
        deps.redis_repository = std::make_shared<repository::InMemoryRedisRepository>();
        auto in_mem_store = std::make_shared<repository::InMemoryActiveRunStore>();
        service_deps.active_run_store = in_mem_store;
        service_deps.radar_transport = std::make_shared<service::running::NullPubSubPublisher>();
        LOG_WARN("Using in-memory persistence plane (dev mode)");
    }

    deps.auth_service = std::make_shared<service::AuthService>(
        deps.user_repository, deps.redis_repository, work_context);

    deps.http_controller = std::make_shared<controller::HttpController>(
        deps.auth_service, deps.user_repository);

    deps.protobuf_controller = std::make_shared<controller::ProtobufController>();

    deps.service_stack = std::make_shared<service::running::ServiceStack>(std::move(service_deps));

    return deps;
}

} // namespace run::server
