#pragma once

#include "logger/logger.hpp"
#include "monitoring/metrics_collector.hpp"
#include "storage/in_memory_lock_free_buffer.hpp"
#include "storage/redis/i_batch_flusher.hpp"
#include "storage/redis/sentinel_config.hpp"

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <vector>

namespace run::storage::redis
{

namespace net = boost::asio;

template <typename T = DeferredTrackEvent>
class GenericDrainWorker : public std::enable_shared_from_this<GenericDrainWorker<T>>
{
public:
    using BufferType = storage::InMemoryLockFreeBuffer<T>;
    using FlusherType = IBatchFlusher<T>;

    GenericDrainWorker(net::any_io_executor executor, std::shared_ptr<BufferType> buffer,
                       std::shared_ptr<FlusherType> flusher, BufferConfig config, std::size_t shard_id = 0)
        : m_executor(std::move(executor)), m_buffer(std::move(buffer)), m_flusher(std::move(flusher)),
          m_config(std::move(config)), m_shard_id(shard_id)
    {
    }

    ~GenericDrainWorker() = default;

    GenericDrainWorker(const GenericDrainWorker&) = delete;
    GenericDrainWorker& operator=(const GenericDrainWorker&) = delete;

    net::awaitable<bool> DrainBatchAsync()
    {
        if (!m_buffer || m_buffer->IsEmpty())
        {
            co_return true;
        }

        std::vector<T> batch;
        batch.reserve(m_config.drain_batch_size);

        for (std::size_t i = 0; i < m_config.drain_batch_size; ++i)
        {
            T item;
            if (!m_buffer->Pop(item))
            {
                break;
            }
            batch.push_back(std::move(item));
        }

        if (batch.empty())
        {
            co_return true;
        }

        const bool success = co_await m_flusher->FlushBatchAsync(batch);
        if (!success)
        {
            LOG_WARN("DrainWorker failed to flush batch of {} items to Redis. Re-enqueuing...", batch.size());
            m_total_failed += batch.size();
            monitoring::MetricsCollector::Instance().IncrementDrainFailures(m_shard_id, batch.size());

            for (auto& item : batch)
            {
                if (!m_buffer->Push(std::move(item)))
                {
                    LOG_ERROR("Buffer full during re-enqueuing of failed drain batch!");
                    monitoring::MetricsCollector::Instance().IncrementDroppedEvents(m_shard_id);
                }
            }
            co_return false;
        }

        m_total_drained += batch.size();
        monitoring::MetricsCollector::Instance().IncrementDrainedEvents(m_shard_id, batch.size());
        LOG_INFO("DrainWorker flushed {} items to Redis Master (total drained: {})", batch.size(),
                 m_total_drained.load());

        co_return true;
    }

    net::awaitable<bool> DrainAllAsync()
    {
        LOG_INFO("DrainWorker: Starting full drain of in-memory buffer to Redis...");

        while (m_buffer && !m_buffer->IsEmpty())
        {
            const bool ok = co_await DrainBatchAsync();
            if (!ok)
            {
                LOG_WARN("DrainWorker: Interrupted full drain due to Redis flush error.");
                co_return false;
            }
        }

        LOG_INFO("DrainWorker: In-memory buffer completely drained!");
        co_return true;
    }

    std::size_t GetTotalDrainedCount() const noexcept
    {
        return m_total_drained.load();
    }

    std::size_t GetTotalFailedCount() const noexcept
    {
        return m_total_failed.load();
    }

    std::size_t GetShardId() const noexcept
    {
        return m_shard_id;
    }

private:
    net::any_io_executor m_executor;
    std::shared_ptr<BufferType> m_buffer;
    std::shared_ptr<FlusherType> m_flusher;
    BufferConfig m_config;
    std::size_t m_shard_id{0};

    std::atomic<std::size_t> m_total_drained{0};
    std::atomic<std::size_t> m_total_failed{0};
};

using DrainWorker = GenericDrainWorker<DeferredTrackEvent>;

} // namespace run::storage::redis
