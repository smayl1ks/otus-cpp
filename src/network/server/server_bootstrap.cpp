#include "network/server/server_bootstrap.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <fmt/format.h>

#include "logger/logger.hpp"
#include "repository/connection_pool.hpp"
#include "repository/postgres_run_history_repository.hpp"
#include "repository/postgres_user_repository.hpp"
#include "storage/redis/redis_active_run_store.hpp"
#include "storage/redis/redis_hex_map_cache.hpp"
#include "storage/redis/redis_pub_sub_publisher.hpp"
#include "storage/redis/redis_repository.hpp"
#include "storage/redis/sentinel_config.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

namespace run::server
{

void InitializePostgresPool(const config::PostgresConfig& config)
{
    const auto pool_size =
        config.pool_size > 0 ? config.pool_size : std::max(1u, std::thread::hardware_concurrency() / 2);

    try
    {
        repository::ConnectionPool::Get().Init(config.ConnectionString(), pool_size);
    }
    catch (const std::exception& e)
    {
        // Fail-fast instead of a silent in-memory dev mode (audit S7): the
        // caller (main) exits non-zero and docker restarts the container.
        throw std::runtime_error(fmt::format("PostgreSQL unreachable after {} connection attempt(s): {}", pool_size,
                                             e.what()));
    }
    LOG_INFO("PostgreSQL connection pool initialized with {} connection(s) to {}:{}", pool_size, config.host,
             config.port);
}

ServerDependencies ServerBootstrap::BuildDependencies(context::WorkContext& work_context,
                                                     boost::asio::any_io_executor io_executor,
                                                     const config::ServerConfig& config)
{
    ServerDependencies deps;

    InitializePostgresPool(config.GetPostgresSettings());
    deps.user_repository = std::make_shared<repository::PostgresUserRepository>(work_context);
    deps.run_history_repository = std::make_shared<repository::PostgresRunHistoryRepository>(work_context);

    service::running::RunningDeps running_deps;
    running_deps.history_repository = deps.run_history_repository;
    running_deps.user_repository = deps.user_repository;
    running_deps.work_context = &work_context;

    auto [redis_cfg, buf_cfg] = storage::redis::LoadStorageConfig("runner.cfg");
    auto redis_manager =
        std::make_shared<storage::redis::ShardedRedisManager>(io_executor, std::move(redis_cfg), buf_cfg);

    // ResilientRedisProducer handles Sentinel failover and circuit breaker buffering
    boost::asio::co_spawn(io_executor, redis_manager->InitializeAsync(), boost::asio::detached);

    deps.redis_repository = std::make_shared<repository::RedisRepository>(redis_manager);
    running_deps.active_run_store = std::make_shared<storage::redis::RedisActiveRunStore>(redis_manager);
    running_deps.radar_transport = std::make_shared<storage::redis::RedisPubSubPublisher>(redis_manager);
    running_deps.hex_map_cache = std::make_shared<storage::redis::RedisHexMapCache>(redis_manager);
    LOG_INFO("Redis storage plane active (stateless node with circuit-breaker buffer)");

    deps.auth_service = std::make_shared<service::AuthService>(deps.user_repository, deps.redis_repository,
                                                               work_context);

    deps.http_controller = std::make_shared<controller::HttpController>(deps.auth_service, deps.user_repository);

    deps.protobuf_controller = std::make_shared<controller::ProtobufController>();

    deps.running_services = std::make_shared<service::running::RunningServices>(std::move(running_deps));

    return deps;
}

} // namespace run::server
