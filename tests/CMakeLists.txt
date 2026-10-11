add_executable(unit_tests
    gps_anticheat_test.cpp
    h3_service_test.cpp
    run_validator_test.cpp
)

target_link_libraries(unit_tests PRIVATE
    ${PROJECT_NAME}_lib
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
