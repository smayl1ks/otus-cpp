add_executable(unit_tests
    boundary_qualifier_test.cpp
    gps_anticheat_test.cpp
    gps_distance_test.cpp
    h3_capture_resolver_test.cpp
    h3_service_test.cpp
    location_mapper_test.cpp
    loop_detector_test.cpp
    map_controller_test.cpp
    metrics_collector_test.cpp
    polygon_geometry_test.cpp
    run_point_test.cpp
    run_validator_test.cpp
    session_manager_zones_test.cpp
)

target_link_libraries(unit_tests PRIVATE
    ${PROJECT_NAME}_core
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)