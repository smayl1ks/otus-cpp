#include "storage/redis/sentinel_config.hpp"
#include "logger/logger.hpp"

#include <boost/program_options.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace run::storage::redis
{

namespace po = boost::program_options;

RedisConfig LoadRedisConfigFromEnv(RedisConfig base)
{
    if (const char* env_num_shards = std::getenv("REDIS_NUM_SHARDS"))
    {
        try
        {
            base.num_shards = std::stoul(env_num_shards);
        }
        catch (...)
        {
            LOG_WARN("Failed to parse REDIS_NUM_SHARDS: '{}'", env_num_shards);
        }
    }

    if (const char* env_pool_size = std::getenv("REDIS_POOL_SIZE"))
    {
        try
        {
            base.pool_size = std::stoul(env_pool_size);
        }
        catch (...)
        {
            LOG_WARN("Failed to parse REDIS_POOL_SIZE: '{}'", env_pool_size);
        }
    }

    if (const char* env_master = std::getenv("REDIS_MASTER_NAME"))
    {
        if (env_master[0] != '\0')
        {
            base.master_name = env_master;
        }
    }

    if (const char* env_sentinels = std::getenv("REDIS_SENTINELS"))
    {
        if (env_sentinels[0] != '\0')
        {
            base.sentinels = ParseSentinelNodes(env_sentinels);
        }
    }

    if (const char* env_pass = std::getenv("REDIS_PASSWORD"))
    {
        base.password = env_pass;
    }

    if (const char* env_conn_to = std::getenv("REDIS_CONNECTION_TIMEOUT_MS"))
    {
        try
        {
            base.connection_timeout = std::chrono::milliseconds(std::stoul(env_conn_to));
        }
        catch (...)
        {
            LOG_WARN("Failed to parse REDIS_CONNECTION_TIMEOUT_MS: '{}'", env_conn_to);
        }
    }

    if (const char* env_cmd_to = std::getenv("REDIS_COMMAND_TIMEOUT_MS"))
    {
        try
        {
            base.command_timeout = std::chrono::milliseconds(std::stoul(env_cmd_to));
        }
        catch (...)
        {
            LOG_WARN("Failed to parse REDIS_COMMAND_TIMEOUT_MS: '{}'", env_cmd_to);
        }
    }

    if (const char* env_ttl_refresh = std::getenv("REDIS_TTL_REFRESH_INTERVAL_SEC"))
    {
        try
        {
            base.ttl_refresh_interval = std::chrono::seconds(std::stoul(env_ttl_refresh));
        }
        catch (...)
        {
            LOG_WARN("Failed to parse REDIS_TTL_REFRESH_INTERVAL_SEC: '{}'", env_ttl_refresh);
        }
    }

    return base;
}

BufferConfig LoadBufferConfigFromEnv(BufferConfig base)
{
    if (const char* env_cap = std::getenv("BUFFER_IN_MEMORY_CAPACITY"))
    {
        try
        {
            base.in_memory_capacity = std::stoul(env_cap);
        }
        catch (...)
        {
            LOG_WARN("Failed to parse BUFFER_IN_MEMORY_CAPACITY: '{}'", env_cap);
        }
    }

    if (const char* env_batch = std::getenv("BUFFER_DRAIN_BATCH_SIZE"))
    {
        try
        {
            base.drain_batch_size = std::stoul(env_batch);
        }
        catch (...)
        {
            LOG_WARN("Failed to parse BUFFER_DRAIN_BATCH_SIZE: '{}'", env_batch);
        }
    }

    return base;
}

std::pair<RedisConfig, BufferConfig> LoadStorageConfig(const std::string& config_file)
{
    RedisConfig redis_cfg;
    BufferConfig buffer_cfg;

    std::string path_to_load = config_file;
    if (!path_to_load.empty() && !std::filesystem::exists(path_to_load))
    {
        if (std::filesystem::exists("../" + path_to_load))
        {
            path_to_load = "../" + path_to_load;
        }
        else if (std::filesystem::exists("../../" + path_to_load))
        {
            path_to_load = "../../" + path_to_load;
        }
    }

    if (path_to_load.empty() || !std::filesystem::exists(path_to_load))
    {
        if (std::filesystem::exists("runner.cfg"))
        {
            path_to_load = "runner.cfg";
        }
        else if (std::filesystem::exists("../runner.cfg"))
        {
            path_to_load = "../runner.cfg";
        }
        else if (std::filesystem::exists("../../runner.cfg"))
        {
            path_to_load = "../../runner.cfg";
        }
        else if (std::filesystem::exists("/etc/runner/runner.cfg"))
        {
            path_to_load = "/etc/runner/runner.cfg";
        }
    }

    if (!path_to_load.empty() && std::filesystem::exists(path_to_load))
    {
        try
        {
            po::options_description redis_desc("Redis options");
            redis_desc.add_options()
                ("redis.num_shards", po::value<std::size_t>(), "Number of shards")
                ("redis.pool_size", po::value<std::size_t>(), "Connection pool size per shard")
                ("redis.master_name", po::value<std::string>(), "Master base name")
                ("redis.sentinels", po::value<std::string>(), "Sentinel nodes comma-separated")
                ("redis.password", po::value<std::string>(), "Redis password")
                ("redis.connection_timeout_ms", po::value<uint64_t>(), "Connection timeout in ms")
                ("redis.command_timeout_ms", po::value<uint64_t>(), "Command timeout in ms")
                ("redis.ttl_refresh_interval_sec", po::value<uint64_t>(), "TTL refresh interval in seconds")
                ("buffer.in_memory_capacity", po::value<std::size_t>(), "In-memory buffer capacity")
                ("buffer.drain_batch_size", po::value<std::size_t>(), "Drain batch size");

            po::variables_map vm;
            std::ifstream ifs(path_to_load);
            if (ifs.is_open())
            {
                po::store(po::parse_config_file(ifs, redis_desc, true), vm);
                po::notify(vm);

                if (vm.count("redis.num_shards"))
                {
                    redis_cfg.num_shards = vm["redis.num_shards"].as<std::size_t>();
                }
                if (vm.count("redis.pool_size"))
                {
                    redis_cfg.pool_size = vm["redis.pool_size"].as<std::size_t>();
                }
                if (vm.count("redis.master_name"))
                {
                    redis_cfg.master_name = vm["redis.master_name"].as<std::string>();
                }
                if (vm.count("redis.sentinels"))
                {
                    redis_cfg.sentinels = ParseSentinelNodes(vm["redis.sentinels"].as<std::string>());
                }
                if (vm.count("redis.password"))
                {
                    redis_cfg.password = vm["redis.password"].as<std::string>();
                }
                if (vm.count("redis.connection_timeout_ms"))
                {
                    redis_cfg.connection_timeout =
                        std::chrono::milliseconds(vm["redis.connection_timeout_ms"].as<uint64_t>());
                }
                if (vm.count("redis.command_timeout_ms"))
                {
                    redis_cfg.command_timeout =
                        std::chrono::milliseconds(vm["redis.command_timeout_ms"].as<uint64_t>());
                }
                if (vm.count("redis.ttl_refresh_interval_sec"))
                {
                    redis_cfg.ttl_refresh_interval =
                        std::chrono::seconds(vm["redis.ttl_refresh_interval_sec"].as<uint64_t>());
                }
                if (vm.count("buffer.in_memory_capacity"))
                {
                    buffer_cfg.in_memory_capacity = vm["buffer.in_memory_capacity"].as<std::size_t>();
                }
                if (vm.count("buffer.drain_batch_size"))
                {
                    buffer_cfg.drain_batch_size = vm["buffer.drain_batch_size"].as<std::size_t>();
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_WARN("Error parsing config file '{}': {}", path_to_load, e.what());
        }
    }

    // Environment variables take precedence over config file
    redis_cfg = LoadRedisConfigFromEnv(redis_cfg);
    buffer_cfg = LoadBufferConfigFromEnv(buffer_cfg);

    return {redis_cfg, buffer_cfg};
}

} // namespace run::storage::redis
