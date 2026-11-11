#include "network/server/server_bootstrap.hpp"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "gameplay/territory_conquest/h3_service.hpp"
#include "logger/logger.hpp"
#include "repository/connection_pool.hpp"
#include "repository/postgres_hex_ownership_source.hpp"
#include "repository/postgres_run_history_repository.hpp"
#include "repository/postgres_user_repository.hpp"
#include "repository/redis_active_run_store.hpp"
#include "repository/redis_hex_map_cache.hpp"
#include "repository/redis_repository.hpp"
#include "service/running/redis_pubsub_publisher.hpp"
#include "service/running/track_flush.hpp"
#include "storage/redis/redis_client.hpp"
#include "storage/redis/sentinel_config.hpp"

namespace run::server
{

namespace
{

run::redis::SentinelConfig MakeSentinelConfig(const config::RedisConfig& redis)
{
    if (redis.num_shards == 0)
    {
        throw std::runtime_error("redis.num-shards must be >= 1");
    }
    if (redis.connection_timeout_ms == 0)
    {
        throw std::runtime_error("redis.connection-timeout-ms must be > 0");
    }

    run::redis::SentinelConfig config;
    config.num_shards = redis.num_shards;
    config.master_prefix = redis.master_prefix;
    config.direct_host = redis.host;
    config.direct_port = std::to_string(redis.port);
    config.password = redis.password;
    config.connection_timeout = std::chrono::milliseconds{redis.connection_timeout_ms};

    // "host:port,host:port"
    std::string_view rest{redis.sentinels};
    while (!rest.empty())
    {
        const std::size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);

        const std::size_t start = item.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos)
        {
            continue;
        }
        const std::size_t end = item.find_last_not_of(" \t\r\n");
        const std::string_view trimmed = item.substr(start, end - start + 1);

        const std::size_t colon = trimmed.rfind(':');
        if (colon == std::string_view::npos)
        {
            config.sentinel_nodes.push_back({std::string{trimmed}, "26379"});
        }
        else
        {
            config.sentinel_nodes.push_back(
                {std::string{trimmed.substr(0, colon)}, std::string{trimmed.substr(colon + 1)}});
        }
    }

    return config;
}

} // namespace

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
        throw std::runtime_error(
            fmt::format("PostgreSQL unreachable after {} connection attempt(s): {}", pool_size, e.what()));
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

    const auto sentinel_config = MakeSentinelConfig(config.GetRedisSettings());

    run::redis::RedisSentinelClientOptions redis_options;
    redis_options.extra_setup_commands.push("FUNCTION", "LOAD", "REPLACE",
                                            std::string{service::running::kTrackFlushFunctionLib});
    redis_options.channel_router = service::running::RouteRadarChannel;
    auto redis_client =
        std::make_shared<run::redis::RedisSentinelClient>(io_executor, sentinel_config, std::move(redis_options));

    deps.redis_repository = std::make_shared<repository::RedisRepository>(redis_client);
    running_deps.active_run_store = std::make_shared<repository::RedisActiveRunStore>(redis_client);
    running_deps.radar_transport = std::make_shared<service::running::RedisPubSubPublisher>(redis_client);

    // Map cache with PostgreSQL self-heal: snapshot holes are re-read from hexagons and written back
    const auto map_cache_h3 = std::make_shared<gameplay::territory_conquest::H3Service>();
    const auto hex_ownership_source = std::make_shared<repository::PostgresHexOwnershipSource>(work_context);
    running_deps.hex_map_cache = std::make_shared<repository::RedisHexMapCache>(
        redis_client, hex_ownership_source,
        [map_cache_h3](uint64_t h3_index) { return map_cache_h3->GetParent(h3_index, 6); });
    deps.redis_sentinel_config = sentinel_config;
    LOG_INFO("Redis storage plane active ({} shard(s), Sentinel: {})", sentinel_config.num_shards,
             sentinel_config.sentinel_nodes.empty() ? "off" : "on");

    deps.auth_service =
        std::make_shared<service::AuthService>(deps.user_repository, deps.redis_repository, work_context);

    deps.http_controller =
        std::make_shared<controller::HttpController>(deps.auth_service, deps.user_repository, hex_ownership_source);

    deps.protobuf_controller = std::make_shared<controller::ProtobufController>();

    deps.running_services = std::make_shared<service::running::RunningServices>(std::move(running_deps));

    return deps;
}

} // namespace run::server
