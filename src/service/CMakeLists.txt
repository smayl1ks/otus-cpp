target_sources(${PROJECT_NAME}_core PRIVATE
    auth_service.cpp
    running/offline_upload_service.cpp
    running/radar_publisher.cpp
    running/run_finalizer.cpp
    running/run_session_service.cpp
    running/telemetry_ingestor.cpp
)
