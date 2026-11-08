#include "gameplay/anticheat/anticheat.hpp"

#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace run::gameplay::anticheat::testing
{

class GpsAntiCheatTest : public ::testing::Test
{
protected:
    GpsAntiCheatConfig m_config{.max_gps_accuracy_meters = 30.0f,
                                .max_running_speed_kmh = 35.0,
                                .max_teleport_distance_m = 100.0,
                                .max_allowed_future_time_ms = 5000,
                                .max_points_per_batch = 5};

    GpsAntiCheat m_validator{m_config};
    const int64_t kBaseTimestampMs = 1700000000000; // Server time

    static TelemetryPoint CreatePoint(double lat, double lon, int64_t timestamp_ms, float accuracy = 5.0f,
                                      bool is_mock = false)
    {
        return TelemetryPoint::Create(lat, lon, timestamp_ms, accuracy, is_mock);
    }
};

TEST_F(GpsAntiCheatTest, ValidateBatch_EmptyBatch_ReturnsOk)
{
    std::vector<TelemetryPoint> batch;
    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::Ok);
}

TEST_F(GpsAntiCheatTest, ValidatePoint_MockLocationFlag_ReturnsMockDetected)
{
    std::vector<TelemetryPoint> batch = {CreatePoint(55.751244, 37.618423, kBaseTimestampMs, 5.0f, true /* is_mock */)};

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::MockDetected);
}

TEST_F(GpsAntiCheatTest, ValidatePoint_LowGpsAccuracy_ReturnsAccuracyLow)
{
    std::vector<TelemetryPoint> batch = {
        CreatePoint(55.751244, 37.618423, kBaseTimestampMs, 45.0f /* exceeds 30m threshold */)};

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::GpsAccuracyLow);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_ExceedsMaxPoints_ReturnsBatchTooLarge)
{
    std::vector<TelemetryPoint> batch;
    for (std::size_t i = 0; i < m_config.max_points_per_batch + 1; ++i)
    {
        batch.push_back(CreatePoint(55.751244, 37.618423, kBaseTimestampMs + i * 1000));
    }

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::BatchTooLarge);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_ValidTrack_ReturnsOk)
{
    std::vector<TelemetryPoint> batch = {
        CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 6000),
        CreatePoint(55.75129, 37.61840, kBaseTimestampMs - 3000),
        CreatePoint(55.75138, 37.61840, kBaseTimestampMs),
    };

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::Ok);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_ExcessiveSpeedVehicle_ReturnsSpeedLimitExceeded)
{
    std::vector<TelemetryPoint> batch = {
        CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 3000),
        CreatePoint(55.76020, 37.61840, kBaseTimestampMs),
    };

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::SpeedLimitExceeded);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_NonMonotonicTimestamps_ReturnsTimeAnomaly)
{
    std::vector<TelemetryPoint> batch = {
        CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 3000),
        CreatePoint(55.75129, 37.61840, kBaseTimestampMs - 6000),
    };

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::InvalidTimestamps);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_DuplicateTimestamps_ReturnsZeroDeltaTime)
{
    std::vector<TelemetryPoint> batch = {
        CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 3000),
        CreatePoint(55.75129, 37.61840, kBaseTimestampMs - 3000),
    };

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::InvalidTimestamps);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_InstantTeleportation_ReturnsTeleportDetected)
{
    std::vector<TelemetryPoint> batch = {
        CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 1000),
        CreatePoint(55.75300, 37.61840, kBaseTimestampMs),
    };

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), AnticheatStatus::TeleportationDetected);
}

TEST_F(GpsAntiCheatTest, ValidatePoint_NanCoordinates_RejectedAsUntrusted)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const TelemetryPoint nan_lat = CreatePoint(nan, 37.618423, kBaseTimestampMs);
    const TelemetryPoint nan_lon = CreatePoint(55.751244, nan, kBaseTimestampMs);
    EXPECT_EQ(m_validator.ValidatePoint(nan_lat, kBaseTimestampMs), AnticheatStatus::GpsAccuracyLow);
    EXPECT_EQ(m_validator.ValidatePoint(nan_lon, kBaseTimestampMs), AnticheatStatus::GpsAccuracyLow);
    const std::vector<TelemetryPoint> batch_with_nan{nan_lon};
    EXPECT_EQ(m_validator.ValidateBatch(batch_with_nan, kBaseTimestampMs), AnticheatStatus::GpsAccuracyLow);
}

TEST_F(GpsAntiCheatTest, ValidatePoint_PastDrift_BoundedTo24h)
{
    const TelemetryPoint too_old = CreatePoint(55.751244, 37.618423, kBaseTimestampMs - 24 * 3600 * 1000 - 1);
    const TelemetryPoint old_but_valid = CreatePoint(55.751244, 37.618423, kBaseTimestampMs - 23 * 3600 * 1000);
    EXPECT_EQ(m_validator.ValidatePoint(too_old, kBaseTimestampMs), AnticheatStatus::InvalidTimestamps);
    EXPECT_EQ(m_validator.ValidatePoint(old_but_valid, kBaseTimestampMs), AnticheatStatus::Ok);
}

TEST_F(GpsAntiCheatTest, ValidateSegment_InterBatchTimeGap_SmallDistance_Passes)
{
    const auto prev = CreatePoint(55.751244, 37.618423, kBaseTimestampMs);
    const auto next = CreatePoint(55.751334, 37.618423, kBaseTimestampMs + 10'000); // ~10 m, 10 s later
    EXPECT_EQ(m_validator.ValidateSegment(prev, next), AnticheatStatus::Ok);
}

TEST_F(GpsAntiCheatTest, ValidateSegment_InterBatchTeleport_Rejected)
{
    const auto prev = CreatePoint(55.751244, 37.618423, kBaseTimestampMs);
    const auto next = CreatePoint(55.769244, 37.618423, kBaseTimestampMs + 5'000);
    EXPECT_EQ(m_validator.ValidateSegment(prev, next), AnticheatStatus::SpeedLimitExceeded);
}

} // namespace run::gameplay::anticheat::testing