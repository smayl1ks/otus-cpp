#include <gtest/gtest.h>

#include "service/running/run_session_service.hpp"
#include "service/running/telemetry_ingestor.hpp"
#include "utils/time.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <map>
#include <set>

#include <algorithm>
#include <future>
#include <span>
#include <vector>

namespace
{

using namespace run::repository;
using run::service::running::RunFinalizer;
using run::service::running::RunSessionService;
using run::service::running::TelemetryIngestor;

namespace net = boost::asio;

/// Runs a coroutine to completion on a private io_context (all fakes are
/// immediately-ready, so no real I/O or threads are involved)
template <typename T> T Await(net::awaitable<T> awaitable)
{
    net::io_context ioc;
    auto result = net::co_spawn(ioc, std::move(awaitable), net::use_future);
    ioc.run();
    return result.get();
}

/// In-memory store with an ordered event log: the closing-race tests assert
/// the exact ORDER of claim/read/append events, not the stored contents
class FakeActiveRunStore final : public IActiveRunStore
{
public:
    std::vector<std::string> events;
    std::map<std::string, ActiveRunRecord> runs;
    std::map<std::string, std::vector<run::gameplay::workout_tracking::RunPoint>> tracks;
    std::set<std::string> closing;

    boost::asio::awaitable<bool> TryClaimUserSlotAsync(int64_t, const std::string&) override { co_return true; }
    boost::asio::awaitable<void> ReleaseUserSlotAsync(int64_t, const std::string&) override { co_return; }
    boost::asio::awaitable<bool> InitActiveRunAsync(const ActiveRunRecord& record) override
    {
        runs[record.session_id] = record;
        co_return true;
    }
    boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunAsync(const std::string& session_id) override
    {
        events.push_back("get_active");
        const auto it = runs.find(session_id);
        if (it == runs.end())
        {
            co_return std::nullopt;
        }
        co_return it->second;
    }
    boost::asio::awaitable<std::optional<ActiveRunRecord>> GetActiveRunByUserAsync(int64_t user_id) override
    {
        for (const auto& [session_id, record] : runs)
        {
            if (record.user_id == user_id)
            {
                co_return record;
            }
        }
        co_return std::nullopt;
    }
    boost::asio::awaitable<void> TouchActiveRunAsync(int64_t, const std::string&) override { co_return; }

    boost::asio::awaitable<bool> ClaimBatchAsync(const std::string&, const std::string&) override
    {
        events.push_back("claim_batch");
        co_return true;
    }
    boost::asio::awaitable<void> ReleaseClaimBatchAsync(const std::string&, const std::string&) override
    {
        co_return;
    }
    boost::asio::awaitable<std::optional<int64_t>>
    AppendTrackBatchAsync(const std::string& session_id, std::span<const run::gameplay::workout_tracking::RunPoint> points,
                          double delta_meters, int64_t sequence_number) override
    {
        events.push_back("append_batch");
        auto& track = tracks[session_id];
        track.insert(track.end(), points.begin(), points.end());
        auto& record = runs.at(session_id);
        record.total_points_count += static_cast<int32_t>(points.size());
        record.total_distance_meters += delta_meters;
        record.last_sequence_number = std::max(record.last_sequence_number, sequence_number);
        co_return static_cast<int64_t>(points.size());
    }
    boost::asio::awaitable<std::vector<run::gameplay::workout_tracking::RunPoint>>
    GetTrackAsync(const std::string& session_id) override
    {
        events.push_back("get_track");
        co_return tracks[session_id];
    }

    boost::asio::awaitable<bool> TryClaimClosingAsync(const std::string& session_id) override
    {
        events.push_back("claim_closing");
        co_return closing.insert(session_id).second;
    }
    boost::asio::awaitable<bool> IsClosingAsync(const std::string& session_id) override
    {
        events.push_back("is_closing");
        co_return closing.contains(session_id);
    }
    boost::asio::awaitable<void> ReleaseClosingAsync(const std::string& session_id) override
    {
        events.push_back("release_closing");
        closing.erase(session_id);
        co_return;
    }
    boost::asio::awaitable<bool> SaveRunSummaryAsync(const StoredRunSummary& summary) override
    {
        summaries[summary.session_id] = summary;
        co_return true;
    }
    boost::asio::awaitable<std::optional<StoredRunSummary>> GetRunSummaryAsync(const std::string& session_id) override
    {
        events.push_back("get_summary");
        const auto it = summaries.find(session_id);
        if (it == summaries.end())
        {
            co_return std::nullopt;
        }
        co_return it->second;
    }

    boost::asio::awaitable<bool> InitUploadAsync(const UploadMetaRecord&) override { co_return true; }
    boost::asio::awaitable<std::optional<UploadMetaRecord>> GetUploadMetaAsync(const std::string&) override
    {
        co_return std::nullopt;
    }
    boost::asio::awaitable<int32_t> PutUploadChunkAsync(const std::string&, int32_t,
                                                        std::span<const run::gameplay::workout_tracking::RunPoint>) override
    {
        co_return 0;
    }
    boost::asio::awaitable<std::vector<int32_t>> GetAcceptedChunksAsync(const std::string&) override
    {
        co_return std::vector<int32_t>{};
    }
    boost::asio::awaitable<std::vector<run::gameplay::workout_tracking::RunPoint>>
    AssembleUploadTrackAsync(const std::string&) override
    {
        co_return std::vector<run::gameplay::workout_tracking::RunPoint>{};
    }
    boost::asio::awaitable<void> DropRunDataAsync(const std::string& session_id, int64_t) override
    {
        events.push_back("drop_run");
        tracks.erase(session_id);
        runs.erase(session_id);
        closing.erase(session_id);
        co_return;
    }

    std::map<std::string, StoredRunSummary> summaries;
};

class FakeHistoryRepository final : public IRunHistoryRepository
{
public:
    boost::asio::awaitable<std::optional<int64_t>> CreateRunAsync(const NewRunRecord&) override
    {
        co_return 1;
    }
    boost::asio::awaitable<FinalizeStatus> FinalizeRunAsync(const StoredRunSummary&,
                                                            std::span<const run::gameplay::workout_tracking::RunPoint>,
                                                            std::vector<HexCaptureRecord>*) override
    {
        co_return FinalizeStatus::kPersisted;
    }
    boost::asio::awaitable<std::optional<StoredRunSummary>> GetFinalizedRunAsync(const std::string&, int64_t) override
    {
        co_return std::nullopt;
    }
};

class FakePubSubPublisher final : public run::service::running::IPubSubPublisher
{
public:
    boost::asio::awaitable<bool> PublishAsync(const std::string&, const std::string&) override { co_return true; }
};

ActiveRunRecord MakeRecord(const std::string& session_id, int64_t user_id = 42)
{
    ActiveRunRecord record;
    record.session_id = session_id;
    record.server_run_id = 1;
    record.user_id = user_id;
    record.started_at_ms = 1'000;
    return record;
}

run::proto::telemetry::LocationBatch MakeBatch(const std::string& session_id, int64_t first_ts_ms)
{
    run::proto::telemetry::LocationBatch batch;
    batch.set_session_id(session_id);
    batch.set_batch_uuid("batch-1");
    batch.set_sequence_number(1);
    batch.set_state(run::proto::telemetry::RUNNING_STATE_ACTIVE);
    for (const int offset_ms : {0, 1'000})
    {
        auto* point = batch.add_points();
        point->set_latitude(55.75 + 0.00001 * (offset_ms / 1'000));
        point->set_longitude(49.10);
        point->set_timestamp(first_ts_ms + offset_ms);
        point->set_speed(2.0);
    }
    return batch;
}

run::service::running::RadarPublisher MakeRadar()
{
    return {std::make_shared<FakePubSubPublisher>(),
            std::make_shared<run::gameplay::territory_conquest::H3Service>()};
}

std::shared_ptr<RunFinalizer> MakeFinalizer(std::shared_ptr<FakeActiveRunStore> store,
                                            std::shared_ptr<FakeHistoryRepository> history)
{
    return std::make_shared<RunFinalizer>(store, history, /*work_context*/ nullptr,
                                          run::gameplay::anticheat::GpsAntiCheatConfig{},
                                          run::gameplay::territory_conquest::H3CaptureConfig{},
                                          run::gameplay::workout_tracking::RunValidationConfig{},
                                          /*hex cache*/ nullptr, /*capture publisher*/ nullptr, /*users*/ nullptr);
}

TEST(RunClosingRace, FinishClaimsClosingBeforeReadingTheTrack)
{
    auto store = std::make_shared<FakeActiveRunStore>();
    auto history = std::make_shared<FakeHistoryRepository>();
    store->runs["sess-1"] = MakeRecord("sess-1");

    RunSessionService service(store, history, MakeFinalizer(store, history));

    run::proto::session::FinishRunRequest request;
    request.set_session_id("sess-1");
    const auto response = Await(service.FinishRunAsync(42, request));

    ASSERT_EQ(response.system_status(), common::SYSTEM_STATUS_OK);
    const auto& ev = store->events;
    const auto claimed = std::find(ev.begin(), ev.end(), "claim_closing");
    const auto track_read = std::find(ev.begin(), ev.end(), "get_track");
    const auto dropped = std::find(ev.begin(), ev.end(), "drop_run");
    ASSERT_NE(claimed, ev.end());
    ASSERT_NE(track_read, ev.end());
    EXPECT_LT(claimed, track_read) << "closing must be claimed before the final track read";
    ASSERT_NE(dropped, ev.end());
    EXPECT_LT(track_read, dropped);
    EXPECT_EQ(ev.back(), "drop_run") << "successful close must clear the closing marker via DropRunDataAsync";
}

TEST(RunClosingRace, IngestorRejectsBatchesOfAClosingSession)
{
    auto store = std::make_shared<FakeActiveRunStore>();
    auto history = std::make_shared<FakeHistoryRepository>();
    store->runs["sess-1"] = MakeRecord("sess-1");
    store->closing.insert("sess-1");

    TelemetryIngestor ingestor(store, std::make_shared<run::service::running::RadarPublisher>(MakeRadar()), {});

    const auto response = Await(ingestor.ProcessBatchAsync(42, MakeBatch("sess-1", 900'000)));

    EXPECT_EQ(response.system_status(), common::SYSTEM_STATUS_SERVICE_UNAVAILABLE);
    const auto& ev = store->events;
    EXPECT_EQ(std::count(ev.begin(), ev.end(), "append_batch"), 0) << "no point may reach the track while closing";
    EXPECT_EQ(std::count(ev.begin(), ev.end(), "claim_batch"), 0) << "no batch dedup key may be burned while closing";
}

TEST(RunClosingRace, IngestorAcceptsBatchesOfAnOpenSession)
{
    auto store = std::make_shared<FakeActiveRunStore>();
    auto history = std::make_shared<FakeHistoryRepository>();
    store->runs["sess-1"] = MakeRecord("sess-1");

    TelemetryIngestor ingestor(store, std::make_shared<run::service::running::RadarPublisher>(MakeRadar()), {});

    const auto response =
        Await(ingestor.ProcessBatchAsync(42, MakeBatch("sess-1", run::utils::NowMs() - 2'000)));

    EXPECT_EQ(response.system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_EQ(response.accepted_points_count(), 2);
    EXPECT_EQ(std::count(store->events.begin(), store->events.end(), "append_batch"), 1);
}

} // namespace
