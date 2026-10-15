#include "network/mappers/location_mapper.hpp"
#include <gtest/gtest.h>

namespace run::network::mappers::testing
{

TEST(LocationMapperTest, ToTelemetryPoint_MapsAllFields)
{
    ::run::proto::location::LocationPoint proto;
    proto.set_latitude(55.7983);
    proto.set_longitude(49.1061);
    proto.set_accuracy(4.2f);
    proto.set_speed(3.5f);
    proto.set_altitude(110.0);
    proto.set_heading(90.0f);
    proto.set_timestamp(1700000000000LL);
    proto.set_is_mock(true);
    proto.set_accelerometer_z(9.81f);

    const auto domain_pt = LocationMapper::ToTelemetryPoint(proto);

    EXPECT_DOUBLE_EQ(domain_pt.coord.latitude, 55.7983);
    EXPECT_DOUBLE_EQ(domain_pt.coord.longitude, 49.1061);
    EXPECT_FLOAT_EQ(domain_pt.accuracy_meters, 4.2f);
    ASSERT_TRUE(domain_pt.speed_ms.has_value());
    EXPECT_FLOAT_EQ(*domain_pt.speed_ms, 3.5f);
    ASSERT_TRUE(domain_pt.altitude_meters.has_value());
    EXPECT_FLOAT_EQ(*domain_pt.altitude_meters, 110.0f);
    ASSERT_TRUE(domain_pt.heading_degrees.has_value());
    EXPECT_FLOAT_EQ(*domain_pt.heading_degrees, 90.0f);
    EXPECT_EQ(domain_pt.RecordedAtMs(), 1700000000000LL);
    EXPECT_TRUE(domain_pt.is_mock);
    EXPECT_FLOAT_EQ(domain_pt.accelerometer_z, 9.81f);
}

TEST(LocationMapperTest, ToTelemetryPoints_ConvertsBatch)
{
    ::run::proto::location::LocationBatch batch;
    batch.set_session_id("session-123");
    auto* pt1 = batch.add_points();
    pt1->set_latitude(55.0);
    pt1->set_longitude(49.0);
    pt1->set_timestamp(1000);

    auto* pt2 = batch.add_points();
    pt2->set_latitude(55.1);
    pt2->set_longitude(49.1);
    pt2->set_timestamp(2000);

    const auto result = LocationMapper::ToTelemetryPoints(batch);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_DOUBLE_EQ(result[0].coord.latitude, 55.0);
    EXPECT_DOUBLE_EQ(result[1].coord.latitude, 55.1);
}

TEST(LocationMapperTest, RunPoint_RoundTripProtobuf)
{
    ::run::proto::location::LocationPoint proto_orig;
    proto_orig.set_latitude(55.8034);
    proto_orig.set_longitude(49.1442);
    proto_orig.set_accuracy(6.0f);
    proto_orig.set_speed(4.1f);
    proto_orig.set_altitude(85.5);
    proto_orig.set_heading(180.0f);
    proto_orig.set_timestamp(1700000005000LL);

    const auto entity = LocationMapper::ToRunPoint(proto_orig, 100, 1, 0x8911062d55bffffULL);
    EXPECT_EQ(entity.run_id, 100);
    EXPECT_EQ(entity.point_order, 1);
    EXPECT_DOUBLE_EQ(entity.coord.latitude, 55.8034);
    EXPECT_DOUBLE_EQ(entity.coord.longitude, 49.1442);
    EXPECT_FLOAT_EQ(entity.accuracy_meters, 6.0f);
    ASSERT_TRUE(entity.speed_ms.has_value());
    EXPECT_FLOAT_EQ(*entity.speed_ms, 4.1f);
    ASSERT_TRUE(entity.altitude_meters.has_value());
    EXPECT_FLOAT_EQ(*entity.altitude_meters, 85.5f);
    ASSERT_TRUE(entity.heading_degrees.has_value());
    EXPECT_FLOAT_EQ(*entity.heading_degrees, 180.0f);
    EXPECT_EQ(entity.RecordedAtMs(), 1700000005000LL);

    ::run::proto::location::LocationPoint proto_converted;
    LocationMapper::ToProtobuf(entity, proto_converted);

    EXPECT_DOUBLE_EQ(proto_converted.latitude(), proto_orig.latitude());
    EXPECT_DOUBLE_EQ(proto_converted.longitude(), proto_orig.longitude());
    EXPECT_FLOAT_EQ(proto_converted.accuracy(), proto_orig.accuracy());
    EXPECT_FLOAT_EQ(proto_converted.speed(), proto_orig.speed());
    EXPECT_DOUBLE_EQ(proto_converted.altitude(), proto_orig.altitude());
    EXPECT_FLOAT_EQ(proto_converted.heading(), proto_orig.heading());
    EXPECT_EQ(proto_converted.timestamp(), proto_orig.timestamp());
}

TEST(LocationMapperTest, AnticheatStatus_RoundTrip)
{
    using DomainStatus = gameplay::anticheat::AnticheatStatus;

    EXPECT_EQ(LocationMapper::ToProto(DomainStatus::Ok), ::common::ANTICHEAT_STATUS_OK);
    EXPECT_EQ(LocationMapper::ToProto(DomainStatus::MockDetected), ::common::ANTICHEAT_STATUS_MOCK_DETECTED);
    EXPECT_EQ(LocationMapper::ToProto(DomainStatus::TeleportationDetected),
              ::common::ANTICHEAT_STATUS_TELEPORTATION_DETECTED);

    EXPECT_EQ(LocationMapper::FromProto(::common::ANTICHEAT_STATUS_OK), DomainStatus::Ok);
    EXPECT_EQ(LocationMapper::FromProto(::common::ANTICHEAT_STATUS_SPEED_LIMIT_EXCEEDED),
              DomainStatus::SpeedLimitExceeded);
}

} // namespace run::network::mappers::testing
