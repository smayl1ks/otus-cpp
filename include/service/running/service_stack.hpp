#pragma once

#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/territory_conquest/capture_resolver.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"
#include "repository/i_active_run_store.hpp"
#include "repository/i_run_history_repository.hpp"
#include "repository/in_memory_stores.hpp"
#include "service/running/offline_upload_service.hpp"
#include "service/running/radar_publisher.hpp"
#include "service/running/run_finalizer.hpp"
#include "service/running/run_session_service.hpp"
#include "service/running/telemetry_ingestor.hpp"

#include <memory>

namespace run::service::running
{

// Everything the service layer needs, wired once at composition root (main,
// or a test). Storage implementations are swapped here — stubs in, Redis/PG
// out — services never change.
struct ServiceDeps
{
    std::shared_ptr<repository::IActiveRunStore> active_run_store;
    std::shared_ptr<repository::IRunHistoryRepository> history_repository;
    std::shared_ptr<IPubSubPublisher> radar_transport;

    gameplay::anticheat::GpsAntiCheatConfig anticheat;
    gameplay::territory_conquest::H3CaptureConfig h3_capture;
    gameplay::workout_tracking::RunValidationConfig validation;
    RadarPublisherConfig radar;
    SessionServiceConfig sessions;
    UploadLimits uploads;
};

// Owns the service graph; EnvelopeDispatcher takes it by reference.
// Declaration order = dependency order (members must not be reordered).
struct ServiceStack
{
    explicit ServiceStack(const ServiceDeps& deps)
        : h3_service(std::make_shared<gameplay::territory_conquest::H3Service>()),
          radar_publisher(std::make_shared<RadarPublisher>(deps.radar_transport, h3_service, deps.radar)),
          run_finalizer(std::make_shared<RunFinalizer>(deps.active_run_store, deps.history_repository, deps.anticheat,
                                                       deps.h3_capture, deps.validation)),
          telemetry_ingestor(deps.active_run_store, radar_publisher, deps.anticheat),
          run_session_service(deps.active_run_store, deps.history_repository, run_finalizer, deps.sessions),
          offline_upload_service(deps.active_run_store, deps.history_repository, run_finalizer, deps.uploads)
    {
    }

    std::shared_ptr<gameplay::territory_conquest::H3Service> h3_service;
    std::shared_ptr<RadarPublisher> radar_publisher;
    std::shared_ptr<RunFinalizer> run_finalizer;

    TelemetryIngestor telemetry_ingestor;
    RunSessionService run_session_service;
    OfflineUploadService offline_upload_service;
};

/// Deps for a fully stubbed stack: in-memory stores + null radar transport.
inline ServiceDeps MakeStubDeps()
{
    ServiceDeps deps;
    auto store = std::make_shared<repository::InMemoryActiveRunStore>();
    auto history = std::make_shared<repository::InMemoryRunHistoryRepository>();
    history->BindFinalizedSink(store.get());
    deps.active_run_store = std::move(store);
    deps.history_repository = std::move(history);
    deps.radar_transport = std::make_shared<NullPubSubPublisher>();
    return deps;
}

} // namespace run::service::running
