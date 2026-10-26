target_sources(${PROJECT_NAME}_lib PRIVATE
    auth_service.cpp
    running/radar_publisher.cpp
    running/telemetry_ingestor.cpp
    running/run_finalizer.cpp
    running/run_session_service.cpp
    running/offline_upload_service.cpp
)
