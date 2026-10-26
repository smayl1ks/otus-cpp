// Pipeline smoke test: runs the full service graph over in-memory stubs —
// through the same EnvelopeDispatcher a WS connection would use.
// Covers: online lifecycle (start -> batches -> finish, idempotent retries),
// cancel, and the offline upload flow (init -> chunks -> commit).

#include "network/server/envelope_dispatcher.hpp"
#include "repository/in_memory_stores.hpp"
#include "service/running/service_stack.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace
{

using run::proto::Envelope;

template <typename T> T RunTask(boost::asio::awaitable<T> task)
{
    boost::asio::io_context ioc;
    auto future = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return future.get();
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

run::proto::telemetry::LocationPoint MakePoint(double lat, double lon, int64_t timestamp_ms)
{
    run::proto::telemetry::LocationPoint point;
    point.set_latitude(lat);
    point.set_longitude(lon);
    point.set_accuracy(5.0f);
    point.set_timestamp(timestamp_ms);
    return point;
}

run::proto::telemetry::LocationBatch MakeBatch(const std::string& session_id, const std::string& batch_uuid,
                                               int64_t sequence_number, int64_t base_ts_ms)
{
    run::proto::telemetry::LocationBatch batch;
    batch.set_session_id(session_id);
    batch.set_batch_uuid(batch_uuid);
    batch.set_sequence_number(sequence_number);
    batch.set_state(run::proto::telemetry::RUNNING_STATE_ACTIVE);
    // 4 points, 20 s apart, ~79 m per step (~4 m/s): inside every anticheat
    // threshold, ~237 m per batch.
    for (int i = 0; i < 4; ++i)
    {
        const double lat = 55.79830 + 0.000611 * i; // ~68 m north
        const double lon = 49.10610 + 0.000360 * i; // ~40 m east
        *batch.add_points() = MakePoint(lat, lon, base_ts_ms + 20'000 * i);
    }
    return batch;
}

Envelope Wrap(Envelope request)
{
    request.set_message_id("msg-" + std::to_string(NowMs()));
    request.set_protocol_version(1);
    return request;
}

struct PipelineFixture
{
    run::service::running::ServiceDeps deps{run::service::running::MakeStubDeps()};
    run::service::running::ServiceStack stack{deps};

    auto& Store()
    {
        return static_cast<run::repository::InMemoryActiveRunStore&>(*deps.active_run_store);
    }
};

Envelope Dispatch(PipelineFixture& f, int64_t user_id, Envelope request)
{
    return RunTask(run::server::DispatchAsync(user_id, Wrap(std::move(request)), f.stack)).value();
}

Envelope MakeStart(const std::string& session_id, run::proto::session::RunMode mode = run::proto::session::RUN_MODE_ONLINE)
{
    Envelope envelope;
    auto& request = *envelope.mutable_start_run_request();
    request.set_session_id(session_id);
    request.set_team_id(1);
    request.set_visibility(run::proto::session::VISIBILITY_PUBLIC);
    request.set_mode(mode);
    request.set_start_timestamp(NowMs());
    return envelope;
}

TEST(ServicePipeline, OnlineLifecycle_StartBatchFinish_IdempotentRetries)
{
    PipelineFixture f;
    const std::string session = "sess-online-1";
    constexpr int64_t kUserId = 42;

    // Start: history row created at once (server_run_id exists from ms zero).
    auto started = Dispatch(f, kUserId, MakeStart(session));
    ASSERT_EQ(started.start_run_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_GT(started.start_run_response().server_run_id(), 0);
    EXPECT_EQ(started.start_run_response().max_points_per_batch(), 100);
    EXPECT_FALSE(started.start_run_response().resumed());

    // Resume with the same session_id: snapshot instead of a second run.
    auto resumed = Dispatch(f, kUserId, MakeStart(session));
    ASSERT_EQ(resumed.start_run_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_TRUE(resumed.start_run_response().resumed());
    EXPECT_EQ(resumed.start_run_response().snapshot().session_id(), session);

    // Second run while the first is active: rejected.
    auto second = Dispatch(f, kUserId, MakeStart("sess-online-2"));
    EXPECT_EQ(second.start_run_response().reject_reason(),
              run::proto::session::START_REJECT_REASON_ACTIVE_SESSION_EXISTS);

    // Telemetry: two accepted batches.
    const int64_t base_ts = NowMs() - 150'000; // whole track in the past, last point ~10 s ago
    auto ack1 = Dispatch(f, kUserId, [&]
                         {
                             Envelope e;
                             *e.mutable_location_batch() = MakeBatch(session, "batch-1", 1, base_ts);
                             return e;
                         }());
    ASSERT_EQ(ack1.batch_process_response().anticheat_status(),
              run::proto::telemetry::ANTICHEAT_STATUS_OK);
    EXPECT_EQ(ack1.batch_process_response().accepted_points_count(), 4);
    EXPECT_GT(ack1.batch_process_response().total_distance_meters(), 0.0);

    auto ack2 = Dispatch(f, kUserId, [&]
                         {
                             Envelope e;
                             *e.mutable_location_batch() = MakeBatch(session, "batch-2", 2, base_ts + 80'000);
                             return e;
                         }());
    EXPECT_EQ(ack2.batch_process_response().total_points_count(), 8);
    EXPECT_GT(ack2.batch_process_response().total_distance_meters(),
              ack1.batch_process_response().total_distance_meters());

    // Batch retry: same uuid -> same OK ack, nothing double-counted.
    auto ack_retry = Dispatch(f, kUserId, [&]
                              {
                                  Envelope e;
                                  *e.mutable_location_batch() = MakeBatch(session, "batch-2", 2, base_ts + 80'000);
                                  return e;
                              }());
    EXPECT_EQ(ack_retry.batch_process_response().total_points_count(), 8);

    // Reconnect sync: server knows the highest sequence number.
    auto sync = Dispatch(f, kUserId, [&]
                         {
                             Envelope e;
                             e.mutable_get_active_run_request();
                             return e;
                         }());
    EXPECT_EQ(sync.get_active_run_response().snapshot().last_sequence_number(), 2);

    // Finish: valid summary, hex captures, points credited.
    auto finished = Dispatch(f, kUserId, [&]
                             {
                                 Envelope e;
                                 auto& request = *e.mutable_finish_run_request();
                                 request.set_session_id(session);
                                 request.set_finish_timestamp(NowMs());
                                 return e;
                             }());
    ASSERT_EQ(finished.finish_run_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_EQ(finished.finish_run_response().reject_reason(),
              run::proto::session::FINISH_REJECT_REASON_NONE);
    EXPECT_GT(finished.finish_run_response().summary().total_distance_meters(), 100.0);
    EXPECT_GT(finished.finish_run_response().summary().hexagons_captured_count(), 0);
    EXPECT_TRUE(finished.finish_run_response().summary().is_valid());
    EXPECT_GT(finished.finish_run_response().summary().total_score(), 0);

    // The history row exists and was finalized; the slot is free again.
    EXPECT_NE(f.Store().FindFinalizedHistory(session), nullptr);

    // Finish retry: ALREADY_FINISHED + the same summary.
    auto finish_retry = Dispatch(f, kUserId, [&]
                                 {
                                     Envelope e;
                                     auto& request = *e.mutable_finish_run_request();
                                     request.set_session_id(session);
                                     request.set_finish_timestamp(NowMs());
                                     return e;
                                 }());
    EXPECT_EQ(finish_retry.finish_run_response().reject_reason(),
              run::proto::session::FINISH_REJECT_REASON_ALREADY_FINISHED);
    EXPECT_DOUBLE_EQ(finish_retry.finish_run_response().summary().total_distance_meters(),
                     finished.finish_run_response().summary().total_distance_meters());

    // Slot released: a new run can start.
    auto next = Dispatch(f, kUserId, MakeStart("sess-online-3"));
    EXPECT_EQ(next.start_run_response().system_status(), common::SYSTEM_STATUS_OK);
}

TEST(ServicePipeline, Cancel_ClosesWithoutCaptures)
{
    PipelineFixture f;
    const std::string session = "sess-cancel";
    constexpr int64_t kUserId = 7;

    ASSERT_EQ(Dispatch(f, kUserId, MakeStart(session)).start_run_response().system_status(),
              common::SYSTEM_STATUS_OK);

    auto cancelled = Dispatch(f, kUserId, [&]
                              {
                                  Envelope e;
                                  auto& request = *e.mutable_cancel_run_request();
                                  request.set_session_id(session);
                                  request.set_client_timestamp(NowMs());
                                  return e;
                              }());
    ASSERT_EQ(cancelled.cancel_run_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_EQ(cancelled.cancel_run_response().summary().status(), run::proto::session::RUN_STATUS_CANCELLED);
    EXPECT_EQ(cancelled.cancel_run_response().summary().total_score(), 0);
}

TEST(ServicePipeline, OfflineUpload_InitChunksOutOfOrderCommit_Idempotent)
{
    PipelineFixture f;
    const std::string session = "sess-offline";
    constexpr int64_t kUserId = 99;
    const int64_t finish_ts = NowMs();
    const int64_t start_ts = finish_ts - 120'000; // 2 min run

    // Init.
    auto init = Dispatch(f, kUserId, [&]
                         {
                             Envelope e;
                             auto& request = *e.mutable_offline_run_init_request();
                             request.set_session_id(session);
                             request.set_team_id(1);
                             request.set_visibility(run::proto::session::VISIBILITY_TEAM_ONLY);
                             request.set_start_timestamp(start_ts);
                             request.set_finish_timestamp(finish_ts);
                             request.set_total_points_count(12);
                             request.set_total_chunks(3);
                             return e;
                         }());
    ASSERT_EQ(init.offline_run_init_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_GT(init.offline_run_init_response().server_run_id(), 0);
    EXPECT_EQ(init.offline_run_init_response().max_points_per_chunk(), 1000);

    // Chunks 1 and 2 of 3 arrive: commit is refused until the set is complete.
    // Points walk ~44 m per 30 s (~1.5 m/s); chunk 1 covers +40 s/+60 s,
    // chunk 2 covers +80 s/+100 s of the run timeline.
    for (const int32_t chunk_index : {1, 2})
    {
        auto chunk = Dispatch(f, kUserId, [&]
                              {
                                  Envelope e;
                                  auto& request = *e.mutable_offline_run_chunk_request();
                                  request.set_session_id(session);
                                  request.set_chunk_index(chunk_index);
                                  request.set_total_chunks(3);
                                  const int64_t offset = 40'000 * chunk_index;
                                  request.add_points()->CopyFrom(MakePoint(55.79830, 49.10610, start_ts + offset));
                                  request.add_points()->CopyFrom(MakePoint(55.79860, 49.10640, start_ts + offset + 20'000));
                                  return e;
                              }());
        ASSERT_EQ(chunk.offline_run_chunk_response().system_status(), common::SYSTEM_STATUS_OK);
        EXPECT_EQ(chunk.offline_run_chunk_response().received_chunks_count(), chunk_index);
    }

    auto early_commit = Dispatch(f, kUserId, [&]
                                 {
                                     Envelope e;
                                     e.mutable_offline_run_commit_request()->set_session_id(session);
                                     return e;
                                 }());
    EXPECT_EQ(early_commit.offline_run_commit_response().reject_reason(),
              run::proto::session::OFFLINE_REJECT_REASON_UPLOAD_NOT_COMPLETE);

    // Init retry mid-upload: resume point (accepted chunk indexes).
    auto init_retry = Dispatch(f, kUserId, [&]
                               {
                                   Envelope e;
                                   auto& request = *e.mutable_offline_run_init_request();
                                   request.set_session_id(session);
                                   request.set_team_id(1);
                                   request.set_visibility(run::proto::session::VISIBILITY_TEAM_ONLY);
                                   request.set_start_timestamp(start_ts);
                                   request.set_finish_timestamp(finish_ts);
                                   request.set_total_points_count(12);
                                   request.set_total_chunks(3);
                                   return e;
                               }());
    ASSERT_EQ(init_retry.offline_run_init_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_EQ(init_retry.offline_run_init_response().accepted_chunk_indexes_size(), 2);

    // Final chunk arrives: commit runs the full finalizer over the track.
    // Chunk 0 covers the beginning of the timeline (+0 s / +20 s).
    auto last_chunk = Dispatch(f, kUserId, [&]
                               {
                                   Envelope e;
                                   auto& request = *e.mutable_offline_run_chunk_request();
                                   request.set_session_id(session);
                                   request.set_chunk_index(0);
                                   request.set_total_chunks(3);
                                   request.add_points()->CopyFrom(MakePoint(55.79830, 49.10610, start_ts));
                                   request.add_points()->CopyFrom(MakePoint(55.79860, 49.10640, start_ts + 20'000));
                                   return e;
                               }());
    ASSERT_EQ(last_chunk.offline_run_chunk_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_EQ(last_chunk.offline_run_chunk_response().received_chunks_count(), 3);

    auto commit = Dispatch(f, kUserId, [&]
                           {
                               Envelope e;
                               e.mutable_offline_run_commit_request()->set_session_id(session);
                               return e;
                           }());
    ASSERT_EQ(commit.offline_run_commit_response().system_status(), common::SYSTEM_STATUS_OK);
    EXPECT_GT(commit.offline_run_commit_response().summary().total_points_count(), 0);
    EXPECT_NE(f.Store().FindFinalizedHistory(session), nullptr);

    // Commit retry: ALREADY_COMMITTED + the stored summary.
    auto commit_retry = Dispatch(f, kUserId, [&]
                                 {
                                     Envelope e;
                                     e.mutable_offline_run_commit_request()->set_session_id(session);
                                     return e;
                                 }());
    EXPECT_EQ(commit_retry.offline_run_commit_response().reject_reason(),
              run::proto::session::OFFLINE_REJECT_REASON_ALREADY_COMMITTED);
    EXPECT_GT(commit_retry.offline_run_commit_response().summary().server_run_id(), 0);
}

TEST(ServicePipeline, ForeignUser_IsRejectedOnEveryPath)
{
    PipelineFixture f;
    const std::string session = "sess-owner";
    constexpr int64_t kOwnerId = 1;
    constexpr int64_t kForeignId = 2;

    ASSERT_EQ(Dispatch(f, kOwnerId, MakeStart(session)).start_run_response().system_status(),
              common::SYSTEM_STATUS_OK);

    Envelope batch;
    *batch.mutable_location_batch() = MakeBatch(session, "foreign-batch", 1, NowMs());
    auto foreign_ack = RunTask(run::server::DispatchAsync(kForeignId, Wrap(std::move(batch)), f.stack)).value();
    EXPECT_EQ(foreign_ack.batch_process_response().system_status(), common::SYSTEM_STATUS_INVALID_PAYLOAD);

    Envelope finish;
    finish.mutable_finish_run_request()->set_session_id(session);
    auto foreign_finish = RunTask(run::server::DispatchAsync(kForeignId, Wrap(std::move(finish)), f.stack)).value();
    EXPECT_EQ(foreign_finish.finish_run_response().reject_reason(),
              run::proto::session::FINISH_REJECT_REASON_SESSION_NOT_FOUND);
}

} // namespace
