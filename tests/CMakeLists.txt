add_executable(unit_tests
    gps_distance_test.cpp
    gps_anticheat_test.cpp
    h3_service_test.cpp
    polygon_geometry_test.cpp
    boundary_qualifier_test.cpp
    loop_detector_test.cpp
    h3_capture_resolver_test.cpp
    run_point_test.cpp
    run_validator_test.cpp
    location_mapper_test.cpp
    service_pipeline_test.cpp
    redis_storage_test.cpp
    metrics_collector_test.cpp
)

target_link_libraries(unit_tests PRIVATE
    ${PROJECT_NAME}_lib
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)