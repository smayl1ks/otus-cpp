#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"

#include "service/run_service.hpp"
#include "storage/in_memory_lock_free_buffer.hpp"
#include "storage/redis/drain_worker.hpp"
#include "storage/redis/i_batch_flusher.hpp"
#include "storage/redis/redis_pub_sub_worker.hpp"
#include "storage/redis/redis_publisher.hpp"
#include "storage/redis/redis_track_repository.hpp"
#include "storage/redis/resilient_redis_producer.hpp"
#include "storage/redis/sentinel_config.hpp"
#include "storage/redis/sharded_redis_manager.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>

using namespace run::storage;
using namespace run::storage::redis;
using namespace run::gameplay::territory_conquest;

TEST(RedisConfigTest, ParseSentinelNodes_SingleAndMultiple)
{
    const std::string config_str = "127.0.0.1:26379, 10.0.0.2:26380, 192.168.1.1";
    const auto nodes = ParseSentinelNodes(config_str);

    ASSERT_EQ(nodes.size(), 3);
    EXPECT_EQ(nodes[0].host, "127.0.0.1");
    EXPECT_EQ(nodes[0].port, "26379");

    EXPECT_EQ(nodes[1].host, "10.0.0.2");
    EXPECT_EQ(nodes[1].port, "26380");

    EXPECT_EQ(nodes[2].host, "192.168.1.1");
    EXPECT_EQ(nodes[2].port, "26379"); // default port
}

TEST(RedisConfigTest, GetShardMasterName_Formatting)
{
    EXPECT_EQ(GetShardMasterName("mymaster", 0, 1), "mymaster");
    EXPECT_EQ(GetShardMasterName("mymaster", 0, 4), "mymaster-0");
    EXPECT_EQ(GetShardMasterName("mymaster", 3, 4), "mymaster-3");
    EXPECT_EQ(GetShardMasterName("", 0, 1), "mymaster");
    EXPECT_EQ(GetShardMasterName("", 2, 4), "mymaster-2");
}

TEST(RedisConfigTest, LoadStorageConfig_FromFileAndEnvOverrides)
{
    // 1. Test config file reading
    auto [redis_cfg, buf_cfg] = LoadStorageConfig("runner.cfg");
    EXPECT_EQ(redis_cfg.num_shards, 1);
    EXPECT_EQ(redis_cfg.pool_size, 2);
    EXPECT_EQ(redis_cfg.master_name, "mymaster");
    EXPECT_EQ(redis_cfg.ttl_refresh_interval.count(), 30);
    EXPECT_EQ(buf_cfg.in_memory_capacity, 16384);
    EXPECT_EQ(buf_cfg.drain_batch_size, 512);

    // 2. Test dynamic environment variable overrides without recompilation
    setenv("REDIS_NUM_SHARDS", "4", 1);
    setenv("REDIS_POOL_SIZE", "3", 1);
    setenv("REDIS_MASTER_NAME", "custom_cluster", 1);
    setenv("REDIS_TTL_REFRESH_INTERVAL_SEC", "45", 1);
    setenv("BUFFER_IN_MEMORY_CAPACITY", "32768", 1);
    setenv("BUFFER_DRAIN_BATCH_SIZE", "1024", 1);

    auto [env_redis, env_buf] = LoadStorageConfig("runner.cfg");
    EXPECT_EQ(env_redis.num_shards, 4);
    EXPECT_EQ(env_redis.pool_size, 3);
    EXPECT_EQ(env_redis.master_name, "custom_cluster");
    EXPECT_EQ(env_redis.ttl_refresh_interval.count(), 45);
    EXPECT_EQ(env_buf.in_memory_capacity, 32768);
    EXPECT_EQ(env_buf.drain_batch_size, 1024);

    unsetenv("REDIS_NUM_SHARDS");
    unsetenv("REDIS_POOL_SIZE");
    unsetenv("REDIS_MASTER_NAME");
    unsetenv("REDIS_TTL_REFRESH_INTERVAL_SEC");
    unsetenv("BUFFER_IN_MEMORY_CAPACITY");
    unsetenv("BUFFER_DRAIN_BATCH_SIZE");
}

TEST(RedisConfigTest, PubSubInstance_ConfigAndEnvOverrides)
{
    // Default: no dedicated instance (legacy shard fallback)
    RedisConfig default_cfg;
    EXPECT_TRUE(default_cfg.pubsub_host.empty());
    EXPECT_TRUE(default_cfg.pubsub_hosts.empty());
    EXPECT_EQ(default_cfg.pubsub_port, "6379");

    setenv("REDIS_PUBSUB_HOSTS", "redis_pubsub_0:6379, redis_pubsub_1:6380, redis_pubsub_2", 1);
    setenv("REDIS_PUBSUB_PASSWORD", "pubsub_secret", 1);

    const auto cfg = LoadRedisConfigFromEnv();
    ASSERT_EQ(cfg.pubsub_hosts.size(), 3u);
    EXPECT_EQ(cfg.pubsub_hosts[0], std::make_pair(std::string{"redis_pubsub_0"}, std::string{"6379"}));
    EXPECT_EQ(cfg.pubsub_hosts[1], std::make_pair(std::string{"redis_pubsub_1"}, std::string{"6380"}));
    EXPECT_EQ(cfg.pubsub_hosts[2], std::make_pair(std::string{"redis_pubsub_2"}, std::string{"6379"}));
    EXPECT_EQ(cfg.pubsub_password, "pubsub_secret");

    unsetenv("REDIS_PUBSUB_HOSTS");
    unsetenv("REDIS_PUBSUB_PASSWORD");
}

TEST(RedisConfigTest, ParsePubSubHosts_SingleAndMultiple)
{
    const auto hosts = ParsePubSubHosts("a:7000, b,  10.0.0.1:7001 ,");
    ASSERT_EQ(hosts.size(), 3u);
    EXPECT_EQ(hosts[0], std::make_pair(std::string{"a"}, std::string{"7000"}));
    EXPECT_EQ(hosts[1], std::make_pair(std::string{"b"}, std::string{"6379"})); // default port
    EXPECT_EQ(hosts[2], std::make_pair(std::string{"10.0.0.1"}, std::string{"7001"}));

    EXPECT_TRUE(ParsePubSubHosts("").empty());
    EXPECT_TRUE(ParsePubSubHosts("  ,  ").empty());
}

TEST(RedisConnectionPoolTest, SetNxAsync_DegradedReturnsUnknown)
{
    boost::asio::io_context ioc;

    RedisConfig cfg{.master_name = "mymaster", .num_shards = 1};
    RedisConnectionPool pool(ioc.get_executor(), cfg);
    // Not connected: outcome must be "unknown", not "duplicate" (false) -
    // the caller proceeds on nullopt, duplicate detection is best-effort

    bool done = false;
    std::optional<bool> result;
    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            result = co_await pool.SetNxAsync("run:batch:1:u", "1", std::chrono::hours(1));
            done = true;
            co_return;
        },
        boost::asio::detached);
    ioc.run();

    EXPECT_TRUE(done);
    EXPECT_FALSE(result.has_value());
}

TEST(RedisPublisherTest, UnconfiguredPublisher_IdlesAndFailsPublish)
{
    boost::asio::io_context ioc;

    RedisConfig cfg; // pubsub_host empty
    RedisPublisher publisher(ioc.get_executor(), cfg);

    EXPECT_FALSE(publisher.IsConfigured());
    EXPECT_FALSE(publisher.IsConnected());

    publisher.Start(); // must not spawn anything, must not crash

    bool publish_failed = false;
    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            publish_failed = !(co_await publisher.PublishAsync("kazan:zone:1:radar:public", "{}"));
            co_return;
        },
        boost::asio::detached);
    ioc.run();

    EXPECT_TRUE(publish_failed);
}

TEST(ShardedRedisManagerTest, PubSubInstanceRouting_PrefersDedicatedInstance)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "9999"}},
        .num_shards = 2,
        .pubsub_host = "127.0.0.1",
        .pubsub_port = "6390",
    };

    ShardedRedisManager manager(ioc.get_executor(), redis_cfg, BufferConfig{});

    ASSERT_EQ(manager.GetPublisherCount(), 1u);
    ASSERT_NE(manager.GetPublisher(), nullptr);
    EXPECT_TRUE(manager.GetPublisher()->IsConfigured());
    EXPECT_EQ(manager.GetPublisher()->GetAddress().first, "127.0.0.1");
    EXPECT_EQ(manager.GetPublisher()->GetAddress().second, "6390");

    // Not connected (nothing listens on 6390): publish is a no-op failure,
    // but must not crash nor block
    bool publish_done = false;
    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            publish_done = true;
            co_await manager.PublishAsync("kazan:zone:2:radar:public", "ping");
            co_return;
        },
        boost::asio::detached);
    ioc.run();
    EXPECT_TRUE(publish_done);
}

TEST(ShardedRedisManagerTest, PubSubFleet_ChannelShardingByH3Zone)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "9999"}},
        .num_shards = 1,
        .pubsub_hosts = {{"127.0.0.1", "6390"}, {"127.0.0.1", "6391"}, {"127.0.0.1", "6392"}, {"127.0.0.1", "6393"}},
    };

    ShardedRedisManager manager(ioc.get_executor(), redis_cfg, BufferConfig{});

    // Fleet of one publisher per configured instance
    ASSERT_EQ(manager.GetPublisherCount(), 4u);
    for (std::size_t i = 0; i < manager.GetPublisherCount(); ++i)
    {
        ASSERT_NE(manager.GetPublisher(i), nullptr);
        EXPECT_TRUE(manager.GetPublisher(i)->IsConfigured());
    }
    EXPECT_EQ(manager.GetPublisher(4), nullptr); // out of range

    // Zone channels route deterministically: same zone -> same publisher
    const std::string zone_a = "kazan:zone:8849adbd43:radar:public";
    const std::string zone_b = "kazan:zone:8849adbd47:radar:team:3";
    EXPECT_EQ(manager.GetPublisherIndex(zone_a), manager.GetPublisherIndex(zone_a));
    EXPECT_EQ(manager.GetPublisherIndex(zone_b), manager.GetPublisherIndex(zone_b));

    // All valid publisher indices
    std::set<std::size_t> distinct;
    for (uint64_t zone = 1; zone < 500; ++zone)
    {
        const auto idx = manager.GetPublisherIndex(fmt::format("kazan:zone:{:x}:radar:public", zone));
        EXPECT_LT(idx, 4u);
        distinct.insert(idx);
    }
    // 500 zones spread over 4 instances - every instance should get traffic
    EXPECT_EQ(distinct.size(), 4u);
}

TEST(RedisConnectionPoolTest, ConnectionPoolTopologyAndTTLThrottling)
{
    boost::asio::io_context ioc;

    RedisConfig cfg{
        .master_name = "mymaster",
        .num_shards = 1,
        .pool_size = 3,
        .ttl_refresh_interval = std::chrono::seconds(45),
    };

    RedisConnectionPool pool(ioc.get_executor(), cfg);

    EXPECT_EQ(pool.GetPoolSize(), 3);
    EXPECT_EQ(pool.GetConnectionCount(), 0);
    EXPECT_FALSE(pool.IsConnected());

    // Connect to master creates pool_size multiplexed connections
    pool.ConnectToMaster("127.0.0.1", "6379");
    EXPECT_EQ(pool.GetConnectionCount(), 3);
    EXPECT_TRUE(pool.IsConnected());

    // Hot-path TTL throttling check: first call refreshes, subsequent calls within interval are throttled
    EXPECT_TRUE(pool.ShouldRefreshTtl(101));
    EXPECT_FALSE(pool.ShouldRefreshTtl(101));
    EXPECT_FALSE(pool.ShouldRefreshTtl(101));

    // Different run ID triggers refresh independently
    EXPECT_TRUE(pool.ShouldRefreshTtl(202));
    EXPECT_FALSE(pool.ShouldRefreshTtl(202));

    // Invalidation resets TTL refresh trigger for the given run_id
    pool.InvalidateTtl(101);
    EXPECT_TRUE(pool.ShouldRefreshTtl(101));
}

TEST(InMemoryLockFreeBufferTest, PushPop_MaintainsFIFOOrder)
{
    InMemoryLockFreeBuffer<DeferredTrackEvent> buffer(16);

    EXPECT_TRUE(buffer.IsEmpty());

    DeferredTrackEvent ev1{.run_id = 101, .latitude = 55.79, .longitude = 49.12, .speed = 3.5, .timestamp = 1000};
    DeferredTrackEvent ev2{.run_id = 102, .latitude = 55.80, .longitude = 49.13, .speed = 4.0, .timestamp = 2000};

    EXPECT_TRUE(buffer.Push(ev1));
    EXPECT_TRUE(buffer.Push(ev2));
    EXPECT_FALSE(buffer.IsEmpty());

    DeferredTrackEvent out1;
    EXPECT_TRUE(buffer.Pop(out1));
    EXPECT_EQ(out1.run_id, 101);
    EXPECT_DOUBLE_EQ(out1.latitude, 55.79);
    EXPECT_EQ(out1.timestamp, 1000);

    DeferredTrackEvent out2;
    EXPECT_TRUE(buffer.Pop(out2));
    EXPECT_EQ(out2.run_id, 102);
    EXPECT_DOUBLE_EQ(out2.latitude, 55.80);
    EXPECT_EQ(out2.timestamp, 2000);

    EXPECT_TRUE(buffer.IsEmpty());
}

TEST(InMemoryLockFreeBufferTest, MultiThreadedConcurrentPushPop)
{
    constexpr std::size_t total_items = 10000;
    InMemoryLockFreeBuffer<DeferredTrackEvent> buffer(total_items * 2);

    std::thread producer(
        [&buffer]()
        {
            for (std::size_t i = 1; i <= total_items; ++i)
            {
                DeferredTrackEvent ev{.run_id = i, .timestamp = static_cast<int64_t>(i)};
                while (!buffer.Push(ev))
                {
                    std::this_thread::yield();
                }
            }
        });

    std::vector<uint64_t> popped_ids;
    popped_ids.reserve(total_items);

    std::thread consumer(
        [&buffer, &popped_ids]()
        {
            while (popped_ids.size() < total_items)
            {
                DeferredTrackEvent out;
                if (buffer.Pop(out))
                {
                    popped_ids.push_back(out.run_id);
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        });

    producer.join();
    consumer.join();

    EXPECT_EQ(popped_ids.size(), total_items);
    for (std::size_t i = 0; i < total_items; ++i)
    {
        EXPECT_EQ(popped_ids[i], i + 1);
    }
}

TEST(ZoneChannelHelperTest, FormattingAndExtractionRoundtrip)
{
    const uint64_t hex = 0x88118e667ffffffULL;

    // Default city prefix
    const std::string default_ch = ZoneChannelHelper::FormatChannel(hex);
    EXPECT_EQ(default_ch, "kazan:zone:88118e667ffffff");
    EXPECT_EQ(ZoneChannelHelper::ExtractH3Zone(default_ch), hex);

    // Custom city prefix
    const std::string custom_ch = ZoneChannelHelper::FormatChannel(hex, "moscow");
    EXPECT_EQ(custom_ch, "moscow:zone:88118e667ffffff");
    EXPECT_EQ(ZoneChannelHelper::ExtractH3Zone(custom_ch, "moscow:zone:"), hex);
    EXPECT_EQ(ZoneChannelHelper::ExtractH3Zone(custom_ch, "kazan:zone:"), 0);

    // Invalid format
    EXPECT_EQ(ZoneChannelHelper::ExtractH3Zone("invalid_channel"), 0);
    EXPECT_EQ(ZoneChannelHelper::ExtractH3Zone("kazan:zone:zzzz"), 0);
}

TEST(RedisPubSubWorkerTest, ExtractH3Zone_ValidAndInvalid)
{
    const uint64_t expected_zone = 0x88118e667ffffffULL;
    const std::string valid_channel = "kazan:zone:88118e667ffffff";

    EXPECT_EQ(RedisPubSubWorker::ExtractH3Zone(valid_channel), expected_zone);
    EXPECT_EQ(RedisPubSubWorker::ExtractH3Zone("moscow:zone:88118e667ffffff"), 0);
    EXPECT_EQ(RedisPubSubWorker::ExtractH3Zone("kazan:zone:not_a_hex"), 0);
}

// Mock flusher to test GenericDrainWorker
class MockTrackFlusher : public IBatchFlusher<DeferredTrackEvent>
{
public:
    bool should_succeed{true};
    std::vector<DeferredTrackEvent> flushed_events;

    boost::asio::awaitable<bool> FlushBatchAsync(const std::vector<DeferredTrackEvent>& batch) override
    {
        if (should_succeed)
        {
            flushed_events.insert(flushed_events.end(), batch.begin(), batch.end());
            co_return true;
        }
        co_return false;
    }
};

TEST(GenericDrainWorkerTest, DrainsBufferedEventsViaFlusher)
{
    boost::asio::io_context ioc;

    auto buffer = std::make_shared<InMemoryLockFreeBuffer<DeferredTrackEvent>>(16);
    auto flusher = std::make_shared<MockTrackFlusher>();

    buffer->Push(DeferredTrackEvent{.run_id = 1, .timestamp = 100});
    buffer->Push(DeferredTrackEvent{.run_id = 2, .timestamp = 200});

    BufferConfig cfg{.in_memory_capacity = 16, .drain_batch_size = 10};

    auto worker = std::make_shared<DrainWorker>(ioc.get_executor(), buffer, flusher, cfg, 0);

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            const bool ok = co_await worker->DrainBatchAsync();
            EXPECT_TRUE(ok);
            EXPECT_EQ(worker->GetTotalDrainedCount(), 2);
            EXPECT_TRUE(buffer->IsEmpty());
            EXPECT_EQ(flusher->flushed_events.size(), 2);
            EXPECT_EQ(flusher->flushed_events[0].run_id, 1);
            EXPECT_EQ(flusher->flushed_events[1].run_id, 2);
            co_return;
        },
        boost::asio::detached);

    ioc.run();
}

TEST(GenericDrainWorkerTest, ReEnqueuesOnFlushFailure)
{
    boost::asio::io_context ioc;

    auto buffer = std::make_shared<InMemoryLockFreeBuffer<DeferredTrackEvent>>(16);
    auto flusher = std::make_shared<MockTrackFlusher>();
    flusher->should_succeed = false;

    buffer->Push(DeferredTrackEvent{.run_id = 42, .timestamp = 100});

    BufferConfig cfg{.in_memory_capacity = 16, .drain_batch_size = 10};

    auto worker = std::make_shared<DrainWorker>(ioc.get_executor(), buffer, flusher, cfg, 1);

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            const bool ok = co_await worker->DrainBatchAsync();
            EXPECT_FALSE(ok);
            EXPECT_EQ(worker->GetTotalFailedCount(), 1);
            // Must have re-enqueued the event
            EXPECT_FALSE(buffer->IsEmpty());
            DeferredTrackEvent out;
            EXPECT_TRUE(buffer->Pop(out));
            EXPECT_EQ(out.run_id, 42);
            co_return;
        },
        boost::asio::detached);

    ioc.run();
}

TEST(ResilientRedisProducerTest, FallbackToBufferWhenDegraded)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "9999"}},
        .password = "",
        .connection_timeout = std::chrono::milliseconds(50),
        .command_timeout = std::chrono::milliseconds(50),
    };

    BufferConfig buffer_cfg{
        .in_memory_capacity = 1024,
        .drain_batch_size = 64,
    };

    auto producer = std::make_shared<ResilientRedisProducer>(ioc.get_executor(), redis_cfg, buffer_cfg);

    EXPECT_EQ(producer->GetCircuitState(), CircuitState::Degraded);

    DeferredTrackEvent ev{
        .run_id = 42,
        .latitude = 55.75,
        .longitude = 37.61,
        .speed = 4.2,
        .timestamp = 1729000000,
        .delta_meters = 15.5,
        .delta_points = 10,
    };

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            const bool ok = co_await producer->PushTrackEventAsync(ev);
            EXPECT_TRUE(ok);
            co_return;
        },
        boost::asio::detached);

    ioc.run();

    auto buffer = producer->GetBuffer();
    ASSERT_NE(buffer, nullptr);
    EXPECT_FALSE(buffer->IsEmpty());

    DeferredTrackEvent popped;
    EXPECT_TRUE(buffer->Pop(popped));
    EXPECT_EQ(popped.run_id, 42);
    EXPECT_DOUBLE_EQ(popped.latitude, 55.75);
    EXPECT_DOUBLE_EQ(popped.delta_meters, 15.5);
    EXPECT_EQ(popped.delta_points, 10);
}

TEST(ShardedRedisManagerTest, HashDistribution_EvenlyAcrossShards)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "26379"}},
        .num_shards = 4,
    };

    BufferConfig buffer_cfg{
        .in_memory_capacity = 512,
        .drain_batch_size = 64,
    };

    ShardedRedisManager manager(ioc.get_executor(), redis_cfg, buffer_cfg);

    EXPECT_EQ(manager.GetShardCount(), 4);

    std::set<std::size_t> hit_shards;
    for (uint64_t run_id = 1; run_id <= 100; ++run_id)
    {
        hit_shards.insert(manager.GetShardIndex(run_id));
    }

    // All 4 shards must receive hashed traffic
    EXPECT_EQ(hit_shards.size(), 4);
}

TEST(ShardedRedisManagerTest, PushTrackEvent_RoutesToDegradedShardBuffer)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "9999"}}, // Unreachable
        .num_shards = 4,
    };

    BufferConfig buffer_cfg{
        .in_memory_capacity = 512,
        .drain_batch_size = 64,
    };

    auto manager = std::make_shared<ShardedRedisManager>(ioc.get_executor(), redis_cfg, buffer_cfg);

    DeferredTrackEvent ev{
        .run_id = 12345,
        .latitude = 55.79,
        .longitude = 49.12,
        .speed = 5.0,
        .timestamp = 1729001000,
        .delta_meters = 25.0,
        .delta_points = 5,
    };

    const std::size_t expected_shard = manager->GetShardIndex(ev.run_id);

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            const bool ok = co_await manager->PushTrackEventAsync(ev);
            EXPECT_TRUE(ok);
            co_return;
        },
        boost::asio::detached);

    ioc.run();

    auto target_shard = manager->GetShard(expected_shard);
    ASSERT_NE(target_shard, nullptr);
    EXPECT_EQ(target_shard->GetCircuitState(), CircuitState::Degraded);

    auto buffer = target_shard->GetBuffer();
    ASSERT_NE(buffer, nullptr);
    EXPECT_FALSE(buffer->IsEmpty());

    DeferredTrackEvent popped;
    EXPECT_TRUE(buffer->Pop(popped));
    EXPECT_EQ(popped.run_id, 12345);
    EXPECT_DOUBLE_EQ(popped.latitude, 55.79);
    EXPECT_DOUBLE_EQ(popped.delta_meters, 25.0);
}

TEST(RedisTrackRepositoryTest, RoutesTelemetryPointToDegradedShardBuffer)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "9999"}}, // Unreachable
        .num_shards = 2,
    };

    BufferConfig buffer_cfg{
        .in_memory_capacity = 64,
        .drain_batch_size = 16,
    };

    auto manager = std::make_shared<ShardedRedisManager>(ioc.get_executor(), redis_cfg, buffer_cfg);
    auto repo = std::make_shared<RedisTrackRepository>(manager);

    auto pt = run::gameplay::workout_tracking::RunPoint::Create(999, 1, 55.75, 37.61, 1729000000000LL, 0, 1.0f, 4.5f);

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            const bool ok = co_await repo->PushTrackPointAsync(999, pt, 12.5, 3);
            EXPECT_TRUE(ok);
            co_return;
        },
        boost::asio::detached);

    ioc.run();

    const std::size_t target_shard = manager->GetShardIndex(999);
    auto buffer = manager->GetShard(target_shard)->GetBuffer();
    ASSERT_NE(buffer, nullptr);
    EXPECT_FALSE(buffer->IsEmpty());

    DeferredTrackEvent popped;
    EXPECT_TRUE(buffer->Pop(popped));
    EXPECT_EQ(popped.run_id, 999);
    EXPECT_DOUBLE_EQ(popped.latitude, 55.75);
    EXPECT_DOUBLE_EQ(popped.delta_meters, 12.5);
    EXPECT_EQ(popped.delta_points, 3);
}

TEST(ResilientRedisProducerTest, PushTrackBatchAsync_BuffersWhenDegraded)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "9999"}},
        .num_shards = 1,
    };
    BufferConfig buffer_cfg{
        .in_memory_capacity = 100,
        .drain_batch_size = 20,
    };

    auto producer = std::make_shared<ResilientRedisProducer>(ioc.get_executor(), redis_cfg, buffer_cfg);
    EXPECT_EQ(producer->GetCircuitState(), CircuitState::Degraded);

    std::vector<DeferredTrackEvent> batch = {
        DeferredTrackEvent{.run_id = 1, .timestamp = 1000},
        DeferredTrackEvent{.run_id = 1, .timestamp = 2000},
        DeferredTrackEvent{.run_id = 1, .timestamp = 3000}
    };

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            const bool ok = co_await producer->PushTrackBatchAsync(std::move(batch));
            EXPECT_TRUE(ok);
            co_return;
        },
        boost::asio::detached);

    ioc.run();

    auto buffer = producer->GetBuffer();
    ASSERT_NE(buffer, nullptr);

    DeferredTrackEvent e1, e2, e3;
    EXPECT_TRUE(buffer->Pop(e1));
    EXPECT_EQ(e1.timestamp, 1000);
    EXPECT_TRUE(buffer->Pop(e2));
    EXPECT_EQ(e2.timestamp, 2000);
    EXPECT_TRUE(buffer->Pop(e3));
    EXPECT_EQ(e3.timestamp, 3000);
    EXPECT_TRUE(buffer->IsEmpty());
}

TEST(RedisTrackRepositoryTest, PushTrackBatchAsync_BatchesPointsIntoDegradedBuffer)
{
    boost::asio::io_context ioc;

    RedisConfig redis_cfg{
        .master_name = "mymaster",
        .sentinels = {{"127.0.0.1", "9999"}},
        .num_shards = 2,
    };
    BufferConfig buffer_cfg{
        .in_memory_capacity = 64,
        .drain_batch_size = 16,
    };

    auto manager = std::make_shared<ShardedRedisManager>(ioc.get_executor(), redis_cfg, buffer_cfg);
    auto repo = std::make_shared<RedisTrackRepository>(manager);

    std::vector<run::gameplay::workout_tracking::RunPoint> points = {
        run::gameplay::workout_tracking::RunPoint::Create(555, 1, 55.75, 37.61, 1000, 0, 1.0f, 4.0f),
        run::gameplay::workout_tracking::RunPoint::Create(555, 2, 55.76, 37.62, 2000, 0, 1.0f, 4.2f),
        run::gameplay::workout_tracking::RunPoint::Create(555, 3, 55.77, 37.63, 3000, 0, 1.0f, 4.5f)
    };

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void>
        {
            const bool ok = co_await repo->PushTrackBatchAsync(555, points, 300.0, 15);
            EXPECT_TRUE(ok);
            co_return;
        },
        boost::asio::detached);

    ioc.run();

    const std::size_t shard_idx = manager->GetShardIndex(555);
    auto buffer = manager->GetShard(shard_idx)->GetBuffer();
    ASSERT_NE(buffer, nullptr);

    DeferredTrackEvent p1, p2, p3;
    EXPECT_TRUE(buffer->Pop(p1));
    EXPECT_EQ(p1.run_id, 555);
    EXPECT_DOUBLE_EQ(p1.delta_meters, 100.0);
    EXPECT_EQ(p1.delta_points, 0);

    EXPECT_TRUE(buffer->Pop(p2));
    EXPECT_EQ(p2.run_id, 555);
    EXPECT_DOUBLE_EQ(p2.delta_meters, 100.0);
    EXPECT_EQ(p2.delta_points, 0);

    EXPECT_TRUE(buffer->Pop(p3));
    EXPECT_EQ(p3.run_id, 555);
    EXPECT_DOUBLE_EQ(p3.delta_meters, 100.0);
    EXPECT_EQ(p3.delta_points, 15);
}

class MockTrackRepository : public run::repository::ITrackRepository
{
public:
    bool has_active{false};
    std::vector<run::gameplay::workout_tracking::RunPoint> pushed_points;
    std::optional<run::repository::ActiveRunStats> stats;
    bool finalized{false};

    boost::asio::awaitable<bool> PushTrackPointAsync(uint64_t run_id,
                                                     const run::gameplay::workout_tracking::RunPoint& point,
                                                     double delta_meters = 0.0, int32_t delta_points = 0) override
    {
        pushed_points.push_back(point);
        co_return true;
    }

    boost::asio::awaitable<bool> PushTrackBatchAsync(uint64_t run_id,
                                                     std::span<const run::gameplay::workout_tracking::RunPoint> points,
                                                     double total_delta_meters = 0.0,
                                                     int32_t total_delta_points = 0) override
    {
        for (const auto& p : points)
        {
            pushed_points.push_back(p);
        }
        co_return true;
    }

    boost::asio::awaitable<bool> InitActiveRunAsync(const std::string& session_id, int64_t user_id, int64_t team_id,
                                                    int64_t start_timestamp) override
    {
        has_active = true;
        stats = run::repository::ActiveRunStats{.user_id = user_id,
                                                .team_id = team_id,
                                                .start_timestamp = start_timestamp,
                                                .distance_meters = 1500.0,
                                                .points_count = 10,
                                                .status = "active"};
        co_return true;
    }

    boost::asio::awaitable<std::optional<run::repository::ActiveRunStats>>
    GetActiveRunStatsAsync(const std::string& session_id) override
    {
        co_return stats;
    }

    boost::asio::awaitable<bool> CheckActiveSessionExistsAsync(int64_t user_id) override
    {
        co_return has_active;
    }

    boost::asio::awaitable<bool> FinalizeActiveRunAsync(const std::string& session_id, int64_t user_id) override
    {
        has_active = false;
        finalized = true;
        co_return true;
    }

    boost::asio::awaitable<std::vector<run::gameplay::workout_tracking::RunPoint>>
    GetTrackAsync(const std::string& session_id) override
    {
        co_return stored_track;
    }

    std::vector<run::gameplay::workout_tracking::RunPoint> stored_track;
};

// TEST(RunServiceIntegrationTest, FullLifecycleWithDecoupledTrackRepository)
// {
//     boost::asio::io_context ioc;

//     auto mock_repo = std::make_shared<MockTrackRepository>();
//     run::service::RunService service(mock_repo);

//     boost::asio::co_spawn(
//         ioc,
//         [&]() -> boost::asio::awaitable<void>
//         {
//             // 1. Start Run
//             ::events::StartRunRequest start_req;
//             start_req.set_session_id("test_session_100");
//             start_req.set_team_id(1);

//             auto start_res = co_await service.StartRunAsync(42, start_req);
//             EXPECT_EQ(start_res.system_status, ::common::SYSTEM_STATUS_OK);
//             EXPECT_EQ(start_res.reject_reason, ::events::START_REJECT_REASON_NONE);
//             EXPECT_EQ(start_res.response_payload.session_id(), "test_session_100");
//             EXPECT_GT(start_res.response_payload.server_start_timestamp(), 0);
//             EXPECT_TRUE(mock_repo->has_active);

//             // 2. Reject duplicate active run
//             auto duplicate_res = co_await service.StartRunAsync(42, start_req);
//             EXPECT_EQ(duplicate_res.reject_reason, ::events::START_REJECT_REASON_ACTIVE_SESSION_EXISTS);

//             // 3. Process Batch
//             run::proto::location::LocationBatch batch;
//             batch.set_batch_uuid("batch-uuid-1");
//             batch.set_session_id("test_session_100");
//             auto* p1 = batch.add_points();
//             p1->set_latitude(55.79);
//             p1->set_longitude(49.12);
//             p1->set_speed(3.5f);
//             p1->set_timestamp(start_res.response_payload.server_start_timestamp() + 1000);

//             auto batch_res = co_await service.ProcessBatchAsync(42, batch);
//             EXPECT_EQ(batch_res.system_status, ::common::SYSTEM_STATUS_OK);
//             EXPECT_EQ(mock_repo->pushed_points.size(), 1);

//             // 4. Finish Run
//             ::events::FinishRunRequest finish_req;
//             finish_req.set_session_id("test_session_100");
//             finish_req.set_finish_timestamp(start_res.response_payload.server_start_timestamp() + 300000); // 5 mins

//             auto finish_res = co_await service.FinishRunAsync(42, finish_req);
//             EXPECT_EQ(finish_res.system_status, ::common::SYSTEM_STATUS_OK);
//             EXPECT_EQ(finish_res.reject_reason, ::events::FINISH_REJECT_REASON_NONE);
//             EXPECT_DOUBLE_EQ(finish_res.response_payload.total_distance_meters(), 1500.0);
//             EXPECT_EQ(finish_res.response_payload.hexes_claimed_count(), 8);
//             EXPECT_TRUE(mock_repo->finalized);
//             EXPECT_FALSE(mock_repo->has_active);

//             co_return;
//         },
//         boost::asio::detached);

//     ioc.run();
// }
