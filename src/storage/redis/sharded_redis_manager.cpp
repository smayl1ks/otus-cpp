
#include "storage/redis/sharded_redis_manager.hpp"
#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "logger/logger.hpp"

namespace run::storage::redis
{

ShardedRedisManager::ShardedRedisManager(net::any_io_executor executor, RedisConfig redis_cfg, BufferConfig buffer_cfg)
    : m_executor(executor), m_base_redis_config(std::move(redis_cfg)), m_buffer_config(buffer_cfg)
{
    const std::size_t num_shards = std::max<std::size_t>(1, m_base_redis_config.num_shards);
    m_shards.reserve(num_shards);

    for (std::size_t i = 0; i < num_shards; ++i)
    {
        RedisConfig shard_cfg = m_base_redis_config;
        shard_cfg.master_name = GetShardMasterName(m_base_redis_config.master_name, i, num_shards);
        shard_cfg.num_shards = 1;

        auto producer = std::make_shared<ResilientRedisProducer>(executor, std::move(shard_cfg), buffer_cfg, i);
        m_shards.push_back(std::move(producer));
    }

    // Pub/Sub fleet: sharded list > single instance > legacy shard fallback
    if (!m_base_redis_config.pubsub_hosts.empty())
    {
        m_publishers.reserve(m_base_redis_config.pubsub_hosts.size());
        for (const auto& [host, port] : m_base_redis_config.pubsub_hosts)
        {
            RedisConfig pub_cfg = m_base_redis_config;
            pub_cfg.pubsub_host = host;
            pub_cfg.pubsub_port = port;
            m_publishers.push_back(std::make_shared<RedisPublisher>(executor, pub_cfg));
        }
    }
    else if (!m_base_redis_config.pubsub_host.empty())
    {
        m_publishers.push_back(std::make_shared<RedisPublisher>(executor, m_base_redis_config));
    }

    LOG_INFO("Configured ShardedRedisManager with {} shard(s) for base master '{}'{}", num_shards,
             m_base_redis_config.master_name,
             m_publishers.empty() ? ""
             : m_publishers.size() == 1
                 ? fmt::format(" and dedicated pub/sub instance at {}:{}",
                               m_base_redis_config.pubsub_host, m_base_redis_config.pubsub_port)
                 : fmt::format(" and dedicated pub/sub fleet of {} instance(s)",
                               m_publishers.size()));
}

net::awaitable<bool> ShardedRedisManager::InitializeAsync()
{
    LOG_INFO("Initializing all {} Redis Sentinel shard(s)...", m_shards.size());

    for (auto& publisher : m_publishers)
    {
        publisher->Start();
    }

    bool all_ok = true;
    for (std::size_t i = 0; i < m_shards.size(); ++i)
    {
        LOG_INFO("Initializing Redis shard [{}/{}]...", i + 1, m_shards.size());
        const bool ok = co_await m_shards[i]->InitializeAsync();
        if (!ok)
        {
            LOG_WARN("Redis shard [{}] failed initial resolution (operating in degraded mode)", i);
            all_ok = false;
        }
    }

    LOG_INFO("ShardedRedisManager initialization complete. Ready to serve traffic across {} shard(s).",
             m_shards.size());
    co_return all_ok;
}

void ShardedRedisManager::Stop()
{
    for (auto& publisher : m_publishers)
    {
        publisher->Stop();
    }

    for (auto& shard : m_shards)
    {
        if (shard)
        {
            shard->Stop();
        }
    }
}

net::awaitable<bool> ShardedRedisManager::PushTrackEventAsync(DeferredTrackEvent event)
{
    const std::size_t shard_idx = (event.session_id[0] != '\0')
                                      ? GetShardIndex(std::string_view(event.session_id))
                                      : GetShardIndex(event.run_id);
    co_return co_await m_shards[shard_idx]->PushTrackEventAsync(event);
}

net::awaitable<bool> ShardedRedisManager::PushTrackBatchAsync(std::vector<DeferredTrackEvent> events)
{
    if (events.empty())
    {
        co_return true;
    }

    if (m_shards.empty())
    {
        co_return false;
    }

    const std::string first_id = (events[0].session_id[0] != '\0') ? std::string(events[0].session_id)
                                                                    : std::to_string(events[0].run_id);
    bool single_run = true;
    for (std::size_t i = 1; i < events.size(); ++i)
    {
        const std::string cur_id = (events[i].session_id[0] != '\0') ? std::string(events[i].session_id)
                                                                     : std::to_string(events[i].run_id);
        if (cur_id != first_id)
        {
            single_run = false;
            break;
        }
    }

    if (single_run)
    {
        const std::size_t shard_idx = (events[0].session_id[0] != '\0')
                                          ? GetShardIndex(std::string_view(events[0].session_id))
                                          : GetShardIndex(events[0].run_id);
        co_return co_await m_shards[shard_idx]->PushTrackBatchAsync(std::move(events));
    }

    std::unordered_map<std::size_t, std::vector<DeferredTrackEvent>> shard_batches;
    for (auto& ev : events)
    {
        const std::size_t s_idx = (ev.session_id[0] != '\0') ? GetShardIndex(std::string_view(ev.session_id))
                                                             : GetShardIndex(ev.run_id);
        shard_batches[s_idx].push_back(std::move(ev));
    }

    bool all_ok = true;
    for (auto& [shard_idx, batch] : shard_batches)
    {
        const bool ok = co_await m_shards[shard_idx]->PushTrackBatchAsync(std::move(batch));
        if (!ok)
        {
            all_ok = false;
        }
    }

    co_return all_ok;
}

net::awaitable<bool> ShardedRedisManager::SetWsTicketAsync(const std::string& ticket, uint64_t user_id,
                                                           std::chrono::seconds ttl)
{
    const std::size_t shard_idx = GetShardIndex(ticket);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->SetWsTicketAsync(ticket, user_id, ttl);
}

net::awaitable<std::optional<uint64_t>> ShardedRedisManager::ValidateWsTicketAsync(const std::string& ticket)
{
    const std::size_t shard_idx = GetShardIndex(ticket);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->ValidateWsTicketAsync(ticket);
}

std::size_t ShardedRedisManager::GetPublisherIndex(const std::string& channel) const noexcept
{
    if (m_publishers.size() <= 1)
    {
        return 0;
    }

    // Zone channels route by the H3 cell itself so that all publishers and all
    // subscriber fleets agree on where a zone lives; everything else by name
    if (channel.starts_with("kazan:zone:"))
    {
        const uint64_t zone = gameplay::territory_conquest::ZoneChannelHelper::ExtractH3Zone(channel);
        return static_cast<std::size_t>(Hash64(zone)) % m_publishers.size();
    }
    return static_cast<std::size_t>(HashString(channel)) % m_publishers.size();
}

net::awaitable<bool> ShardedRedisManager::PublishAsync(const std::string& channel, const std::string& payload)
{
    // Dedicated radar fleet first; fall back to shard masters in legacy mode
    if (!m_publishers.empty())
    {
        co_return co_await m_publishers[GetPublisherIndex(channel)]->PublishAsync(channel, payload);
    }

    const std::size_t shard_idx = GetShardIndex(channel);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->PublishAsync(channel, payload);
}

std::shared_ptr<RedisPublisher> ShardedRedisManager::GetPublisher(std::size_t index) const noexcept
{
    if (index < m_publishers.size())
    {
        return m_publishers[index];
    }
    return nullptr;
}

std::size_t ShardedRedisManager::GetPublisherCount() const noexcept
{
    return m_publishers.size();
}

net::awaitable<bool> ShardedRedisManager::SetAsync(const std::string& key, const std::string& value,
                                                   std::chrono::seconds ttl)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->SetAsync(key, value, ttl);
}

net::awaitable<std::optional<bool>> ShardedRedisManager::SetNxAsync(const std::string& key,
                                                                     const std::string& value,
                                                                     std::chrono::seconds ttl)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->SetNxAsync(key, value, ttl);
}

net::awaitable<std::optional<std::string>> ShardedRedisManager::GetAsync(const std::string& key)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->GetAsync(key);
}

net::awaitable<bool> ShardedRedisManager::DeleteAsync(const std::string& key)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->DeleteAsync(key);
}

net::awaitable<bool> ShardedRedisManager::ExistsAsync(const std::string& key)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->ExistsAsync(key);
}

net::awaitable<bool> ShardedRedisManager::HSetAsync(const std::string& key, const std::string& field,
                                                    const std::string& value, std::optional<std::chrono::seconds> ttl)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->HSetAsync(key, field, value, ttl);
}

net::awaitable<bool> ShardedRedisManager::HSetMultipleAsync(
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& fields,
    std::optional<std::chrono::seconds> ttl)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->HSetMultipleAsync(key, fields, ttl);
}

net::awaitable<bool> ShardedRedisManager::HSetMultipleAsync(
    const std::string& key,
    const std::vector<std::string>& flattened_fields,
    std::optional<std::chrono::seconds> ttl)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->HSetMultipleAsync(key, flattened_fields, ttl);
}

void ShardedRedisManager::InvalidateTtl(uint64_t run_id)
{
    const std::size_t shard_idx = GetShardIndex(run_id);
    if (shard_idx < m_shards.size() && m_shards[shard_idx] && m_shards[shard_idx]->GetRedisPool())
    {
        m_shards[shard_idx]->GetRedisPool()->InvalidateTtl(run_id);
    }
}

net::awaitable<std::optional<std::string>> ShardedRedisManager::HGetAsync(const std::string& key,
                                                                          const std::string& field)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->HGetAsync(key, field);
}

net::awaitable<std::unordered_map<std::string, std::string>> ShardedRedisManager::HGetAllAsync(const std::string& key)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->HGetAllAsync(key);
}

net::awaitable<std::vector<std::string>> ShardedRedisManager::LRangeAsync(const std::string& key, int64_t start,
                                                                          int64_t stop)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->LRangeAsync(key, start, stop);
}

net::awaitable<bool> ShardedRedisManager::RPushRangeAsync(const std::string& key,
                                                           const std::vector<std::string>& values,
                                                           std::optional<std::chrono::seconds> ttl)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->RPushRangeAsync(key, values, ttl);
}

net::awaitable<int64_t> ShardedRedisManager::SAddAsync(const std::string& key, const std::string& member,
                                                      std::optional<std::chrono::seconds> ttl)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->SAddAsync(key, member, ttl);
}

net::awaitable<std::vector<std::string>> ShardedRedisManager::SMembersAsync(const std::string& key)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->SMembersAsync(key);
}

net::awaitable<int64_t> ShardedRedisManager::SCardAsync(const std::string& key)
{
    const std::size_t shard_idx = GetShardIndex(key);
    co_return co_await m_shards[shard_idx]->GetRedisPool()->SCardAsync(key);
}

std::size_t ShardedRedisManager::GetShardIndex(uint64_t key) const noexcept
{
    if (m_shards.size() <= 1)
    {
        return 0;
    }
    return Hash64(key) % m_shards.size();
}

std::size_t ShardedRedisManager::GetShardIndex(std::string_view key) const noexcept
{
    if (m_shards.size() <= 1)
    {
        return 0;
    }
    return HashString(key) % m_shards.size();
}

std::shared_ptr<ResilientRedisProducer> ShardedRedisManager::GetShard(std::size_t index) const
{
    if (index < m_shards.size())
    {
        return m_shards[index];
    }
    return nullptr;
}

std::size_t ShardedRedisManager::GetShardCount() const noexcept
{
    return m_shards.size();
}

std::vector<CircuitState> ShardedRedisManager::GetCircuitStates() const
{
    std::vector<CircuitState> states;
    states.reserve(m_shards.size());
    for (const auto& shard : m_shards)
    {
        states.push_back(shard ? shard->GetCircuitState() : CircuitState::Degraded);
    }
    return states;
}

std::size_t ShardedRedisManager::GetTotalDroppedEvents() const noexcept
{
    std::size_t total = 0;
    for (const auto& shard : m_shards)
    {
        if (shard)
        {
            total += shard->GetDroppedEventsCount();
        }
    }
    return total;
}

} // namespace run::storage::redis
