#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "repository/i_active_run_store.hpp"
#include "service/running/radar_publisher.hpp"
#include "telemetry.pb.h"

#include <boost/asio/awaitable.hpp>
#include <memory>

namespace run::service::running
{

// Hot path of the whole backend: one call per telemetry batch. Contract:
// never touches PostgreSQL, never blocks on radar, all-or-nothing batch
// ingestion. Everything is keyed by session_id.
class TelemetryIngestor
{
public:
    TelemetryIngestor(std::shared_ptr<repository::IActiveRunStore> active_run_store,
                      std::shared_ptr<RadarPublisher> radar_publisher,
                      gameplay::anticheat::GpsAntiCheatConfig anticheat_config = {});

    boost::asio::awaitable<run::proto::telemetry::BatchProcessResponse>
    ProcessBatchAsync(int64_t user_id, const run::proto::telemetry::LocationBatch& batch);

private:
    static int64_t NowMs() noexcept;

    std::shared_ptr<repository::IActiveRunStore> m_store;
    std::shared_ptr<RadarPublisher> m_radar;
    gameplay::anticheat::GpsAntiCheat m_anticheat;
};

} // namespace run::service::running
