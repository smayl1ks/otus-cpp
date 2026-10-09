#include "domain/anticheat.hpp"

#include "common.pb.h"
#include "location.pb.h"

#include <gtest/gtest.h>

namespace run::domain::anticheat::testing
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

    /**
     * @brief Create a mock LocationPoint protobuf message.
     */
    static run::proto::location::LocationPoint CreatePoint(double lat, double lon, int64_t timestamp_ms,
                                                           float accuracy = 5.0f, bool is_mock = false)
    {
        run::proto::location::LocationPoint point;
        point.set_latitude(lat);
        point.set_longitude(lon);
        point.set_timestamp(timestamp_ms);
        point.set_accuracy(accuracy);
        point.set_is_mock(is_mock);
        return point;
    }
};

TEST_F(GpsAntiCheatTest, ValidateBatch_EmptyBatch_ReturnsOk)
{
    run::proto::location::LocationBatch batch;
    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_OK);
}

TEST_F(GpsAntiCheatTest, ValidatePoint_MockLocationFlag_ReturnsMockDetected)
{
    run::proto::location::LocationBatch batch;
    *batch.add_points() = CreatePoint(55.751244, 37.618423, kBaseTimestampMs, 5.0f, true /* is_mock */);

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_MOCK_DETECTED);
}

TEST_F(GpsAntiCheatTest, ValidatePoint_LowGpsAccuracy_ReturnsAccuracyLow)
{
    run::proto::location::LocationBatch batch;
    *batch.add_points() = CreatePoint(55.751244, 37.618423, kBaseTimestampMs, 45.0f /* exceeds 30m threshold */);

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_GPS_ACCURACY_LOW);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_ExceedsMaxPoints_ReturnsBatchTooLarge)
{
    run::proto::location::LocationBatch batch;
    for (std::size_t i = 0; i < m_config.max_points_per_batch + 1; ++i)
    {
        *batch.add_points() = CreatePoint(55.751244, 37.618423, kBaseTimestampMs + i * 1000);
    }

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_BATCH_TOO_LARGE);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_ValidTrack_ReturnsOk)
{
    run::proto::location::LocationBatch batch;

    // Normal runner trajectory ~12 km/h (3.33 m/s)
    // Moving ~10 meters every 3 seconds (~0.00009 degrees latitude)
    *batch.add_points() = CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 6000);
    *batch.add_points() = CreatePoint(55.75129, 37.61840, kBaseTimestampMs - 3000);
    *batch.add_points() = CreatePoint(55.75138, 37.61840, kBaseTimestampMs);

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_OK);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_ExcessiveSpeedVehicle_ReturnsSpeedLimitExceeded)
{
    run::proto::location::LocationBatch batch;
    *batch.add_points() = CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 3000);
    *batch.add_points() = CreatePoint(55.76020, 37.61840, kBaseTimestampMs);

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_SPEED_LIMIT_EXCEEDED);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_NonMonotonicTimestamps_ReturnsTimeAnomaly)
{
    run::proto::location::LocationBatch batch;
    *batch.add_points() = CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 3000);
    *batch.add_points() = CreatePoint(55.75129, 37.61840, kBaseTimestampMs - 6000);

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_INVALID_TIMESTAMPS);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_DuplicateTimestamps_ReturnsZeroDeltaTime)
{
    run::proto::location::LocationBatch batch;
    *batch.add_points() = CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 3000);
    *batch.add_points() = CreatePoint(55.75129, 37.61840, kBaseTimestampMs - 3000); // delta_t = 0

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_INVALID_TIMESTAMPS);
}

TEST_F(GpsAntiCheatTest, ValidateBatch_InstantTeleportation_ReturnsTeleportDetected)
{
    run::proto::location::LocationBatch batch;

    *batch.add_points() = CreatePoint(55.75120, 37.61840, kBaseTimestampMs - 1000);
    *batch.add_points() = CreatePoint(55.75300, 37.61840, kBaseTimestampMs);

    EXPECT_EQ(m_validator.ValidateBatch(batch, kBaseTimestampMs), common::ANTICHEAT_STATUS_TELEPORTATION_DETECTED);
}

} // namespace run::domain::anticheat::testing