#pragma once

#include "context/work_context.hpp"
#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/territory_conquest/capture_resolver.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"
#include "network/mappers/capture_event_mapper.hpp"
#include "repository/i_active_run_store.hpp"
#include "repository/i_hex_map_cache.hpp"
#include "repository/i_run_history_repository.hpp"
#include "repository/iuser_repository.hpp"
#include "service/running/offline_upload_service.hpp"
#include "service/running/radar_publisher.hpp"
#include "service/running/run_finalizer.hpp"
#include "service/running/run_session_service.hpp"
#include "service/running/telemetry_ingestor.hpp"

#include <memory>

namespace run::service::running
{

/// Application dependencies wired at composition root
struct RunningDeps
{
    std::shared_ptr<repository::IActiveRunStore> active_run_store;
    std::shared_ptr<repository::IRunHistoryRepository> history_repository;
    std::shared_ptr<IPubSubPublisher> radar_transport;
    std::shared_ptr<repository::IHexMapCache> hex_map_cache;
    std::shared_ptr<repository::IUserRepository> user_repository;

    /// Worker pool for anticheat replay + H3 capture
    context::WorkContext* work_context{nullptr};

    gameplay::anticheat::GpsAntiCheatConfig anticheat;
    gameplay::territory_conquest::H3CaptureConfig h3_capture;
    gameplay::workout_tracking::RunValidationConfig validation;
    RadarPublisherConfig radar;
    SessionServiceConfig sessions;
    UploadLimits uploads;
};

/// Owns the running-domain service graph
struct RunningServices
{
    explicit RunningServices(const RunningDeps& deps)
        : h3_service(std::make_shared<gameplay::territory_conquest::H3Service>()), hex_map_cache(deps.hex_map_cache),
          capture_event_publisher(std::make_shared<network::mappers::CaptureEventPublisher>(deps.radar_transport)),
          radar_publisher(std::make_shared<RadarPublisher>(deps.radar_transport, h3_service, deps.radar)),
          run_finalizer(std::make_shared<RunFinalizer>(
              deps.active_run_store, deps.history_repository, deps.work_context, deps.anticheat, deps.h3_capture,
              deps.validation, deps.hex_map_cache, capture_event_publisher, deps.user_repository)),
          telemetry_ingestor(deps.active_run_store, radar_publisher, deps.anticheat),
          run_session_service(deps.active_run_store, deps.history_repository, run_finalizer, deps.sessions),
          offline_upload_service(deps.active_run_store, deps.history_repository, run_finalizer, deps.uploads),
          user_repository(deps.user_repository)
    {
    }

    std::shared_ptr<gameplay::territory_conquest::H3Service> h3_service;
    std::shared_ptr<repository::IHexMapCache> hex_map_cache;
    std::shared_ptr<ICaptureEventPublisher> capture_event_publisher;
    std::shared_ptr<RadarPublisher> radar_publisher;
    std::shared_ptr<RunFinalizer> run_finalizer;
    // Profile source for confirmed team membership (radar fan-out filter)
    std::shared_ptr<repository::IUserRepository> user_repository;

    TelemetryIngestor telemetry_ingestor;
    RunSessionService run_session_service;
    OfflineUploadService offline_upload_service;
};

} // namespace run::service::running
