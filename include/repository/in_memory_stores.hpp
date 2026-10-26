#pragma once

// In-memory implementations of the persistence interfaces. They stand in for
// Redis / PostgreSQL while the storage layer is being built and serve as the
// reference behavior for the future production implementations (same
// idempotency semantics, same key layout in comments).

#include "repository/i_active_run_store.hpp"
#include "repository/i_run_history_repository.hpp"

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace run::repository
{

namespace detail
{
inline boost::asio::awaitable<bool> Resolved(bool value)
{
    co_return value;
}
} // namespace detail

class InMemoryActiveRunStore final : public IActiveRunStore
{
public:
    // ---- Lifecycle -------------------------------------------------------

    boost::asio::awaitable<bool> TryClaimUserSlotAsync(int64_t user_id, const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        auto [it, inserted] = m_user_slots.try_emplace(user_id, session_id);
        co_return inserted || it->second == session_id;
    }

    boost::asio::awaitable<void> ReleaseUserSlotAsync(int64_t user_id, const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_user_slots.find(user_id); it != m_user_slots.end() && it->second == session_id)
        {
            m_user_slots.erase(it);
        }
        co_return;
    }

    boost::asio::awaitable<bool> InitActiveRunAsync(const ActiveRunRecord& record) override
    {
        std::lock_guard lock(m_mutex);
        m_active_runs[record.session_id] = record;
        m_tracks.erase(record.session_id); // resume must not mix with a stale track
        co_return true;
    }

    boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunAsync(const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_active_runs.find(session_id);
        if (it == m_active_runs.end())
        {
            co_return std::nullopt;
        }
        co_return it->second;
    }

    boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunByUserAsync(int64_t user_id) override
    {
        std::lock_guard lock(m_mutex);
        const auto slot = m_user_slots.find(user_id);
        if (slot == m_user_slots.end())
        {
            co_return std::nullopt;
        }
        const auto it = m_active_runs.find(slot->second);
        co_return it != m_active_runs.end() ? std::optional(it->second) : std::nullopt;
    }

    // ---- Telemetry ingestion ----------------------------------------------

    boost::asio::awaitable<bool> ClaimBatchAsync(const std::string& session_id, const std::string& batch_uuid) override
    {
        std::lock_guard lock(m_mutex);
        return detail::Resolved(m_claimed_batches.emplace(session_id + ":" + batch_uuid).second);
    }

    boost::asio::awaitable<bool> AppendTrackBatchAsync(const std::string& session_id,
                                                       std::span<const gameplay::workout_tracking::RunPoint> points,
                                                       double delta_meters, int64_t sequence_number) override
    {
        std::lock_guard lock(m_mutex);
        auto it = m_active_runs.find(session_id);
        if (it == m_active_runs.end())
        {
            co_return false;
        }
        auto& track = m_tracks[session_id];
        track.insert(track.end(), points.begin(), points.end());
        auto& record = it->second;
        record.total_distance_meters += delta_meters;
        record.total_points_count += static_cast<int32_t>(points.size());
        record.last_sequence_number = std::max(record.last_sequence_number, sequence_number);
        co_return true;
    }

    boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    GetTrackAsync(const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_tracks.find(session_id);
        co_return it != m_tracks.end() ? it->second : std::vector<gameplay::workout_tracking::RunPoint>{};
    }

    // ---- Idempotent close --------------------------------------------------

    boost::asio::awaitable<bool> SaveRunSummaryAsync(const StoredRunSummary& summary) override
    {
        std::lock_guard lock(m_mutex);
        m_summaries[summary.session_id] = summary;
        co_return true;
    }

    boost::asio::awaitable<std::optional<StoredRunSummary>> GetRunSummaryAsync(const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_summaries.find(session_id);
        if (it == m_summaries.end())
        {
            co_return std::nullopt;
        }
        co_return it->second;
    }

    // ---- Offline upload assembly -------------------------------------------

    boost::asio::awaitable<bool> InitUploadAsync(const UploadMetaRecord& meta) override
    {
        std::lock_guard lock(m_mutex);
        m_upload_meta[meta.session_id] = meta;
        m_upload_chunks[meta.session_id].clear();
        co_return true;
    }

    boost::asio::awaitable<std::optional<UploadMetaRecord>> GetUploadMetaAsync(const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_upload_meta.find(session_id);
        if (it == m_upload_meta.end())
        {
            co_return std::nullopt;
        }
        co_return it->second;
    }

    boost::asio::awaitable<int32_t> PutUploadChunkAsync(const std::string& session_id, int32_t chunk_index,
                                                        std::span<const gameplay::workout_tracking::RunPoint> points)
        override
    {
        std::lock_guard lock(m_mutex);
        auto& chunks = m_upload_chunks[session_id];
        auto [it, inserted] = chunks.try_emplace(chunk_index);
        if (inserted)
        {
            it->second.assign(points.begin(), points.end());
        }
        co_return static_cast<int32_t>(chunks.size());
    }

    boost::asio::awaitable<std::vector<int32_t>> GetAcceptedChunksAsync(const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        std::vector<int32_t> accepted;
        if (const auto it = m_upload_chunks.find(session_id); it != m_upload_chunks.end())
        {
            accepted.reserve(it->second.size());
            for (const auto& [chunk_index, points] : it->second)
            {
                accepted.push_back(chunk_index);
            }
        }
        std::sort(accepted.begin(), accepted.end());
        co_return accepted;
    }

    boost::asio::awaitable<std::vector<gameplay::workout_tracking::RunPoint>>
    AssembleUploadTrackAsync(const std::string& session_id) override
    {
        std::lock_guard lock(m_mutex);
        std::vector<gameplay::workout_tracking::RunPoint> track;
        if (const auto it = m_upload_chunks.find(session_id); it != m_upload_chunks.end())
        {
            for (auto& [chunk_index, points] : it->second) // std::map: chunk_index order
            {
                track.insert(track.end(), points.begin(), points.end());
            }
        }
        co_return track;
    }

    boost::asio::awaitable<void> DropRunDataAsync(const std::string& session_id, int64_t user_id) override
    {
        std::lock_guard lock(m_mutex);
        m_active_runs.erase(session_id);
        m_tracks.erase(session_id);
        m_upload_meta.erase(session_id);
        m_upload_chunks.erase(session_id);
        ReleaseUserSlotUnsafe(user_id, session_id);
        co_return;
    }

    // ---- Test inspection hooks ----------------------------------------------

    const StoredRunSummary* FindSummary(const std::string& session_id) const
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_summaries.find(session_id);
        return it != m_summaries.end() ? &it->second : nullptr;
    }

    const StoredRunSummary* FindFinalizedHistory(const std::string& session_id) const
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_finalized.find(session_id);
        return it != m_finalized.end() ? &it->second : nullptr;
    }

    int64_t NextRunId() const
    {
        std::lock_guard lock(m_mutex);
        return m_next_run_id;
    }

    void PutFinalizedForTest(const std::string& session_id, const StoredRunSummary& summary)
    {
        std::lock_guard lock(m_mutex);
        m_finalized[session_id] = summary;
    }

private:
    void ReleaseUserSlotUnsafe(int64_t user_id, const std::string& session_id)
    {
        if (const auto it = m_user_slots.find(user_id); it != m_user_slots.end() && it->second == session_id)
        {
            m_user_slots.erase(it);
        }
    }

    mutable std::mutex m_mutex;
    std::map<int64_t, std::string> m_user_slots;                    // user:active_run:{user_id}
    std::map<std::string, ActiveRunRecord> m_active_runs;           // run:active:{session_id}
    std::map<std::string, std::vector<gameplay::workout_tracking::RunPoint>> m_tracks; // run:track:{session_id}
    std::set<std::string> m_claimed_batches;                        // run:batch:{session_id}:{batch_uuid}
    std::map<std::string, StoredRunSummary> m_summaries;            // run:finished:{session_id}
    std::map<std::string, UploadMetaRecord> m_upload_meta;          // run:upload:{session_id}
    std::map<std::string, std::map<int32_t, std::vector<gameplay::workout_tracking::RunPoint>>> m_upload_chunks;
    std::map<std::string, StoredRunSummary> m_finalized;            // rows handed to history (test hook)

protected:
    int64_t m_next_run_id{1};
};

class InMemoryRunHistoryRepository final : public IRunHistoryRepository
{
public:
    // Records everything finalized; FindFinalizedHistory above asserts on it.
    void BindFinalizedSink(InMemoryActiveRunStore* sink)
    {
        std::lock_guard lock(m_mutex);
        m_sink = sink;
    }

    boost::asio::awaitable<std::optional<int64_t>> CreateRunAsync(const NewRunRecord& record) override
    {
        std::lock_guard lock(m_mutex);
        m_created_rows.emplace(record.session_id, record);
        co_return m_next_run_id++;
    }

    boost::asio::awaitable<bool> FinalizeRunAsync(const StoredRunSummary& summary,
                                                  std::span<const gameplay::workout_tracking::RunPoint> track) override
    {
        std::lock_guard lock(m_mutex);
        if (!m_created_rows.contains(summary.session_id))
        {
            co_return false; // no runs row to update
        }
        if (m_sink != nullptr)
        {
            m_sink->PutFinalizedForTest(summary.session_id, summary);
        }
        m_finalized_points[summary.session_id].assign(track.begin(), track.end());
        co_return true;
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, NewRunRecord> m_created_rows;
    std::map<std::string, std::vector<gameplay::workout_tracking::RunPoint>> m_finalized_points;
    InMemoryActiveRunStore* m_sink{nullptr};
    int64_t m_next_run_id{1};
};

} // namespace run::repository
