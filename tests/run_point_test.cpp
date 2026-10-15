#include "gameplay/anticheat/anticheat.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace run::gameplay::workout_tracking::testing
{

TEST(RunPointTest, Create_InitializesAllFieldsCorrectly)
{
    const auto pt = RunPoint::Create(
        /*run_id=*/42,
        /*point_order=*/0,
        /*lat=*/55.7983,
        /*lon=*/49.1061,
        /*timestamp_ms=*/1700000000000LL,
        /*h3_index=*/0x8911062d55bffffULL,
        /*accuracy=*/4.5f,
        /*speed=*/3.2f,
        /*altitude=*/120.0f,
        /*steps=*/150);

    EXPECT_EQ(pt.run_id, 42);
    EXPECT_EQ(pt.point_order, 0);
    EXPECT_DOUBLE_EQ(pt.coord.latitude, 55.7983);
    EXPECT_DOUBLE_EQ(pt.coord.longitude, 49.1061);
    EXPECT_EQ(pt.RecordedAtMs(), 1700000000000LL);
    EXPECT_EQ(pt.h3_index, 0x8911062d55bffffULL);
    EXPECT_FLOAT_EQ(pt.accuracy_meters, 4.5f);
    ASSERT_TRUE(pt.speed_ms.has_value());
    EXPECT_FLOAT_EQ(*pt.speed_ms, 3.2f);
    ASSERT_TRUE(pt.altitude_meters.has_value());
    EXPECT_FLOAT_EQ(*pt.altitude_meters, 120.0f);
    ASSERT_TRUE(pt.step_count.has_value());
    EXPECT_EQ(*pt.step_count, 150);
}

TEST(RunPointTest, TelemetryPoint_InitializesCorrectly)
{
    const auto tp = anticheat::TelemetryPoint::Create(
        /*lat=*/55.8034,
        /*lon=*/49.1442,
        /*timestamp_ms=*/1700000005000LL,
        /*accuracy=*/6.0f,
        /*is_mock=*/false,
        /*speed=*/4.1f,
        /*altitude=*/85.5f,
        /*heading=*/180.0f,
        /*accel_z=*/9.81f);

    EXPECT_DOUBLE_EQ(tp.coord.latitude, 55.8034);
    EXPECT_DOUBLE_EQ(tp.coord.longitude, 49.1442);
    EXPECT_EQ(tp.RecordedAtMs(), 1700000005000LL);
    EXPECT_FLOAT_EQ(tp.accuracy_meters, 6.0f);
    EXPECT_FALSE(tp.is_mock);
    ASSERT_TRUE(tp.speed_ms.has_value());
    EXPECT_FLOAT_EQ(*tp.speed_ms, 4.1f);
    ASSERT_TRUE(tp.altitude_meters.has_value());
    EXPECT_FLOAT_EQ(*tp.altitude_meters, 85.5f);
    ASSERT_TRUE(tp.heading_degrees.has_value());
    EXPECT_FLOAT_EQ(*tp.heading_degrees, 180.0f);
    EXPECT_FLOAT_EQ(tp.accelerometer_z, 9.81f);
}

TEST(RunPointTest, RunValidator_ValidatesRunPointSpanDirectly)
{
    RunValidator validator;

    std::vector<RunPoint> track = {
        RunPoint::Create(1, 0, 55.7983, 49.1061, 1000),
        RunPoint::Create(1, 1, 55.7983 + 0.002, 49.1061, 70000), // ~222m in 69s
    };

    auto metrics = validator.Validate(track);
    EXPECT_TRUE(metrics.is_valid);
    EXPECT_GT(metrics.total_distance_meters, 200.0);
    EXPECT_EQ(metrics.duration_seconds, 69);
}

} // namespace run::gameplay::workout_tracking::testing
