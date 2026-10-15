#include "gameplay/workout_tracking/workout_session.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace run::gameplay::workout_tracking::testing
{

class RunValidatorTest : public ::testing::Test
{
protected:
    RunValidator m_validator;

    static ::run::gameplay::workout_tracking::RunPoint CreatePoint(double lat, double lon, int64_t timestamp_ms)
    {
        return ::run::gameplay::workout_tracking::RunPoint::Create(
            /*run_id=*/1, /*point_order=*/0, lat, lon, timestamp_ms);
    }
};

// Test case: Empty or insufficient points should be invalid
TEST_F(RunValidatorTest, EmptyOrSinglePointIsInvalid)
{
    std::vector<::run::gameplay::workout_tracking::RunPoint> points;

    auto metrics_empty = m_validator.Validate(points);
    EXPECT_FALSE(metrics_empty.is_valid);
    EXPECT_DOUBLE_EQ(metrics_empty.total_distance_meters, 0.0);
    EXPECT_EQ(metrics_empty.duration_seconds, 0);

    points.push_back(CreatePoint(55.751244, 49.618438, 1000));
    auto metrics_single = m_validator.Validate(points);
    EXPECT_FALSE(metrics_single.is_valid);
}

// Test case: Run that meets all standard requirements (distance >= 100m, duration >= 60s)
TEST_F(RunValidatorTest, ValidRunPasses)
{
    std::vector<::run::gameplay::workout_tracking::RunPoint> points = {
        CreatePoint(55.0, 49.0, 0),
        CreatePoint(55.0018, 49.0, 120000),
    };

    auto metrics = m_validator.Validate(points);
    EXPECT_TRUE(metrics.is_valid);
    EXPECT_GE(metrics.total_distance_meters, 100.0);
    EXPECT_GE(metrics.duration_seconds, 60);
}

// Test case: Run is too short in distance (< 100m)
TEST_F(RunValidatorTest, TooShortDistanceFails)
{
    std::vector<::run::gameplay::workout_tracking::RunPoint> points = {
        CreatePoint(55.0, 49.0, 0),
        CreatePoint(55.00009, 49.0, 100000),
    };

    auto metrics = m_validator.Validate(points);
    EXPECT_FALSE(metrics.is_valid);
}

// Test case: Run is too short in duration (< 60s)
TEST_F(RunValidatorTest, TooShortDurationFails)
{
    std::vector<::run::gameplay::workout_tracking::RunPoint> points = {
        CreatePoint(55.0, 49.0, 0),
        CreatePoint(55.0045, 49.0, 10000),
    };

    auto metrics = m_validator.Validate(points);
    EXPECT_FALSE(metrics.is_valid);
}

// Test case: Exceeding maximum distance limit (> 150 km)
TEST_F(RunValidatorTest, ExceedsMaxDistanceFails)
{
    RunValidationConfig custom_config;
    custom_config.max_distance_meters = 1000.0; // Set low max for testing (1 km)
    RunValidator custom_validator(custom_config);

    std::vector<::run::gameplay::workout_tracking::RunPoint> points = {
        CreatePoint(55.0, 49.0, 0),
        CreatePoint(55.018, 49.0, 3600000),
    };

    auto metrics = custom_validator.Validate(points);
    EXPECT_FALSE(metrics.is_valid);
}

// Test case: Exceeding maximum duration limit (> 12 hours)
TEST_F(RunValidatorTest, ExceedsMaxDurationFails)
{
    RunValidationConfig custom_config;
    custom_config.max_duration_seconds = 3600; // Set max to 1 hour for testing
    RunValidator custom_validator(custom_config);

    std::vector<::run::gameplay::workout_tracking::RunPoint> points = {
        CreatePoint(55.0, 49.0, 0),
        CreatePoint(55.0045, 49.0, 7200000),
    };

    auto metrics = custom_validator.Validate(points);
    EXPECT_FALSE(metrics.is_valid);
}

} // namespace run::gameplay::workout_tracking::testing
