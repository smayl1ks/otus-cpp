#include "domain/run_validator.hpp"

#include "location.pb.h"
#include <gtest/gtest.h>

namespace run::domain::validator::testing
{

class RunValidatorTest : public ::testing::Test
{
protected:
    RunValidator m_validator;

    run::proto::location::LocationPoint CreatePoint(double lat, double lon, int64_t timestamp_ms)
    {
        run::proto::location::LocationPoint point;
        point.set_latitude(lat);
        point.set_longitude(lon);
        point.set_timestamp(timestamp_ms);
        return point;
    }
};

// Test case: Empty or insufficient points should be invalid
TEST_F(RunValidatorTest, EmptyOrSinglePointIsInvalid)
{
    run::proto::location::LocationBatch batch;

    auto metrics_empty = m_validator.Validate(batch.points());
    EXPECT_FALSE(metrics_empty.is_valid);
    EXPECT_DOUBLE_EQ(metrics_empty.total_distance_meters, 0.0);
    EXPECT_EQ(metrics_empty.duration_seconds, 0);

    *batch.add_points() = CreatePoint(55.751244, 49.618438, 1000);
    auto metrics_single = m_validator.Validate(batch.points());
    EXPECT_FALSE(metrics_single.is_valid);
}

// Test case: Run that meets all standard requirements (distance >= 100m, duration >= 60s)
TEST_F(RunValidatorTest, ValidRunPasses)
{
    run::proto::location::LocationBatch batch;
    // Moving roughly ~200 meters north over 120 seconds
    *batch.add_points() = CreatePoint(55.0, 49.0, 0);
    *batch.add_points() = CreatePoint(55.0018, 49.0, 120000);

    auto metrics = m_validator.Validate(batch.points());
    EXPECT_TRUE(metrics.is_valid);
    EXPECT_GE(metrics.total_distance_meters, 100.0);
    EXPECT_GE(metrics.duration_seconds, 60);
}

// Test case: Run is too short in distance (< 100m)
TEST_F(RunValidatorTest, TooShortDistanceFails)
{
    run::proto::location::LocationBatch batch;
    // Only ~10 meters apart, but duration is 100 seconds
    *batch.add_points() = CreatePoint(55.0, 49.0, 0);
    *batch.add_points() = CreatePoint(55.00009, 49.0, 100000);

    auto metrics = m_validator.Validate(batch.points());
    EXPECT_FALSE(metrics.is_valid);
}

// Test case: Run is too short in duration (< 60s)
TEST_F(RunValidatorTest, TooShortDurationFails)
{
    run::proto::location::LocationBatch batch;
    // Distance is 500 meters, but elapsed time is only 10 seconds (teleport / car speed)
    *batch.add_points() = CreatePoint(55.0, 49.0, 0);
    *batch.add_points() = CreatePoint(55.0045, 49.0, 10000);

    auto metrics = m_validator.Validate(batch.points());
    EXPECT_FALSE(metrics.is_valid);
}

// Test case: Exceeding maximum distance limit (> 150 km)
TEST_F(RunValidatorTest, ExceedsMaxDistanceFails)
{
    RunValidationConfig custom_config;
    custom_config.max_distance_meters = 1000.0; // Set low max for testing (1 km)
    RunValidator custom_validator(custom_config);

    run::proto::location::LocationBatch batch;
    // ~2 kilometers apart over 3600 seconds
    *batch.add_points() = CreatePoint(55.0, 49.0, 0);
    *batch.add_points() = CreatePoint(55.018, 49.0, 3600000);

    auto metrics = custom_validator.Validate(batch.points());
    EXPECT_FALSE(metrics.is_valid);
}

// Test case: Exceeding maximum duration limit (> 12 hours)
TEST_F(RunValidatorTest, ExceedsMaxDurationFails)
{
    RunValidationConfig custom_config;
    custom_config.max_duration_seconds = 3600; // Set max to 1 hour for testing
    RunValidator custom_validator(custom_config);

    run::proto::location::LocationBatch batch;
    // 500 meters distance, but 2 hours elapsed (zombie session / forgot to stop)
    *batch.add_points() = CreatePoint(55.0, 49.0, 0);
    *batch.add_points() = CreatePoint(55.0045, 49.0, 7200000);

    auto metrics = custom_validator.Validate(batch.points());
    EXPECT_FALSE(metrics.is_valid);
}

} // namespace run::domain::validator::testing