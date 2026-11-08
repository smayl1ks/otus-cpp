#include "service/running/telemetry_ingestor.hpp"

#include "logger/logger.hpp"
#include "network/mappers/location_mapper.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <span>

namespace run::service::running
{

namespace
{

using gameplay::anticheat::TelemetryPoint;
using gameplay::workout_tracking::RunPoint;

double DistanceBetweenSpan(std::span<const TelemetryPoint> points)
{
    double total = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i)
    {
        total += gameplay::CalculateGCircleDistance(points[i - 1].coord, points[i].coord);
    }
    return total;
}

} // namespace

TelemetryIngestor::TelemetryIngestor(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                                     std::shared_ptr<RadarPublisher> radar_publisher,
                                     gameplay::anticheat::GpsAntiCheatConfig anticheat_config)
    : m_run_store(std::move(active_run_store)), m_radar(std::move(radar_publisher)), m_anticheat(anticheat_config)
{
}

net::awaitable<run::proto::telemetry::BatchProcessResponse>
TelemetryIngestor::ProcessBatchAsync(int64_t user_id, const run::proto::telemetry::LocationBatch& batch)
{
    run::proto::telemetry::BatchProcessResponse ack;
    ack.set_system_status(common::SYSTEM_STATUS_OK);
    ack.set_anticheat_status(run::proto::telemetry::ANTICHEAT_STATUS_OK);
    ack.set_session_id(batch.session_id());
    ack.set_batch_uuid(batch.batch_uuid());
    ack.set_sequence_number(batch.sequence_number());
    ack.set_server_timestamp_ms(utils::NowMs());

    if (batch.session_id().empty() || batch.batch_uuid().empty())
    {
        ack.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return ack;
    }

    auto record = co_await m_run_store->GetActiveRunAsync(batch.session_id());
    if (!record.has_value() || record->user_id != user_id)
    {
        LOG_WARN("[ingestor] batch for unknown/foreign session '{}' from user {}", batch.session_id(), user_id);
        ack.set_system_status(common::SYSTEM_STATUS_INVALID_PAYLOAD);
        co_return ack;
    }

    if (batch.state() == run::proto::telemetry::RUNNING_STATE_PAUSED && batch.points().empty())
    {
        LOG_DEBUG("[ingestor] heartbeat for session '{}'", batch.session_id());
        ack.set_total_distance_meters(record->total_distance_meters);
        ack.set_total_points_count(record->total_points_count);
        ack.set_last_sequence_number(record->last_sequence_number);
        co_return ack;
    }

    const std::vector<TelemetryPoint> points = network::mappers::LocationMapper::ToTelemetryPoints(batch);
    if (const auto anticheat = m_anticheat.ValidateBatch(points, ack.server_timestamp_ms());
        anticheat != gameplay::anticheat::AnticheatStatus::Ok)
    {
        LOG_INFO("[ingestor] batch '{}' of session '{}' rejected: {}", batch.batch_uuid(), batch.session_id(),
                 gameplay::anticheat::ToString(anticheat));
        ack.set_anticheat_status(network::mappers::LocationMapper::ToProto(anticheat));
        co_return ack;
    }

    if (!(co_await m_run_store->ClaimBatchAsync(batch.session_id(), batch.batch_uuid())))
    {
        LOG_DEBUG("[ingestor] duplicate batch '{}' of session '{}'", batch.batch_uuid(), batch.session_id());
        ack.set_total_distance_meters(record->total_distance_meters);
        ack.set_total_points_count(record->total_points_count);
        ack.set_last_sequence_number(record->last_sequence_number);
        co_return ack;
    }

    if (record->last_point_ts_ms > 0 && !points.empty())
    {
        const auto anchor =
            gameplay::anticheat::TelemetryPoint::Create(record->last_lat, record->last_lon, record->last_point_ts_ms);
        const auto seam = m_anticheat.ValidateSegment(anchor, points.front());
        if (seam != gameplay::anticheat::AnticheatStatus::Ok)
        {
            co_await m_run_store->ReleaseClaimBatchAsync(batch.session_id(), batch.batch_uuid());
            LOG_INFO("[ingestor] batch '{}' of session '{}' rejected at seam: {}", batch.batch_uuid(),
                     batch.session_id(), gameplay::anticheat::ToString(seam));
            ack.set_anticheat_status(network::mappers::LocationMapper::ToProto(seam));
            co_return ack;
        }
    }

    const double delta_meters = DistanceBetweenSpan(points);
    const std::vector<RunPoint> track_points =
        network::mappers::LocationMapper::ToRunPoints(record->server_run_id, points, record->total_points_count);
    if (!(co_await m_run_store->AppendTrackBatchAsync(batch.session_id(), track_points, delta_meters,
                                                      batch.sequence_number())))
    {
        co_await m_run_store->ReleaseClaimBatchAsync(batch.session_id(), batch.batch_uuid());
        ack.set_system_status(common::SYSTEM_STATUS_INTERNAL_SERVER_ERROR);
        ack.set_anticheat_status(run::proto::telemetry::ANTICHEAT_STATUS_UNSPECIFIED);
        co_return ack;
    }

    ack.set_accepted_points_count(static_cast<int32_t>(points.size()));
    ack.set_total_distance_meters(record->total_distance_meters + delta_meters);
    ack.set_total_points_count(record->total_points_count + static_cast<int32_t>(points.size()));
    ack.set_last_sequence_number(std::max(record->last_sequence_number, batch.sequence_number()));

    co_await m_radar->PublishIfNeededAsync(user_id, *record, points, ack.server_timestamp_ms());

    co_return ack;
}

} // namespace run::service::running
