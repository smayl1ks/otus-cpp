#include "domain/h3_capture_resolver.hpp"
#include "domain/h3_service.hpp"
#include "location.pb.h"
#include <gtest/gtest.h>

namespace run::domain::h3::testing
{

class H3CaptureResolverTest : public ::testing::Test
{
protected:
    H3Service m_h3_service{H3Config{.default_resolution = 9}};

    const std::size_t kDefaultMinPoints = 1;

    const double kBaseLat = 55.7983;
    const double kBaseLng = 49.1061;

    void AddPoint(google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points, double lat,
                  double lon, float accuracy = 5.0f, int64_t timestamp = 0, bool is_mock = false) const
    {
        auto* point = points.Add();
        point->set_latitude(lat);
        point->set_longitude(lon);
        point->set_accuracy(accuracy);
        point->set_timestamp(timestamp);
        point->set_is_mock(is_mock);
    }
};

TEST_F(H3CaptureResolverTest, Resolve_EmptyPoints_ReturnsEmptyResult)
{
    H3CaptureResolver resolver(m_h3_service);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    const auto result = resolver.Resolve(points);

    EXPECT_TRUE(result.claimed_hexes.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_MinDistanceFiltering_DropsHexBelowDistanceThreshold)
{
    // Points satisfy min_points_per_hex (2 points), but distance is only ~3 meters (below 15m threshold)
    H3CaptureConfig config{.min_points_per_hex = 2, .min_distance_per_hex_meters = 15.0};
    H3CaptureResolver resolver(m_h3_service, config);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;
    AddPoint(points, kBaseLat, kBaseLng);
    AddPoint(points, kBaseLat + 0.00002, kBaseLng + 0.00002);

    const auto result = resolver.Resolve(points);
    EXPECT_TRUE(result.claimed_hexes.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_MinDistanceFiltering_AcceptsHexAboveDistanceThreshold)
{
    // Points satisfy min_points_per_hex (2 points) and distance is ~40 meters (> 15m threshold)
    H3CaptureConfig config{.min_points_per_hex = 2, .min_distance_per_hex_meters = 15.0};
    H3CaptureResolver resolver(m_h3_service, config);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;
    AddPoint(points, kBaseLat, kBaseLng);
    AddPoint(points, kBaseLat + 0.0003, kBaseLng + 0.0003);

    const auto result = resolver.Resolve(points);
    EXPECT_FALSE(result.claimed_hexes.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_OutAndBackTrack_DoesNotTriggerFakeLoop)
{
    // Runner runs out along a straight street and returns back along the same line
    H3CaptureConfig config{.min_points_per_hex = 2, .min_distance_per_hex_meters = 0.0, .enable_loop_capture = true};
    H3CaptureResolver resolver(m_h3_service, config);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;
    // Going North 400m
    for (int i = 0; i <= 8; ++i)
    {
        AddPoint(points, kBaseLat + (i * 0.0005), kBaseLng);
    }
    // Coming South back to start along the same line
    for (int i = 7; i >= 0; --i)
    {
        AddPoint(points, kBaseLat + (i * 0.0005), kBaseLng);
    }

    const auto detailed = resolver.Resolve(points);

    // Traversed hexes along the street should be captured
    EXPECT_FALSE(detailed.boundary_hexes.empty());
    // Out-and-back has 0 enclosed area, so no enclosed loop hexes should be captured!
    EXPECT_TRUE(detailed.enclosed_hexes.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_CounterClockwiseLoop_ClaimsBoundaryAndEnclosed)
{
    H3CaptureConfig config{.min_points_per_hex = 1, .enable_loop_capture = true};
    H3CaptureResolver resolver(m_h3_service, config);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    // Running Counter-Clockwise around a block (~890m x ~500m): East -> North -> West -> South
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat, kBaseLng + (i * 0.002));
    }
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat + (i * 0.002), kBaseLng + 0.008);
    }
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat + 0.008, kBaseLng + 0.008 - (i * 0.002));
    }
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat + 0.008 - (i * 0.002), kBaseLng);
    }

    const auto detailed = resolver.Resolve(points);

    EXPECT_FALSE(detailed.boundary_hexes.empty());
    EXPECT_FALSE(detailed.enclosed_hexes.empty());
    EXPECT_FALSE(detailed.claimed_hexes.empty());
    EXPECT_GT(detailed.total_distance_meters, 2000.0);
}

TEST_F(H3CaptureResolverTest, Resolve_RealKazanRun_ClaimsBoundaryAndEnclosed)
{
    H3CaptureConfig config{.min_points_per_hex = 2, .min_distance_per_hex_meters = 0.0, .enable_loop_capture = true};
    H3CaptureResolver resolver(m_h3_service, config);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    const double base_lat = 55.8034;
    const double base_lng = 49.1442;

    for (int i = 0; i < 10; ++i)
    {
        AddPoint(points, base_lat + (i * 0.0008), base_lng);
    }

    for (int i = 0; i < 10; ++i)
    {
        AddPoint(points, base_lat + 0.008, base_lng + (i * 0.001));
    }

    for (int i = 0; i < 10; ++i)
    {
        AddPoint(points, base_lat + 0.008 - (i * 0.0008), base_lng + 0.010);
    }

    for (int i = 0; i < 10; ++i)
    {
        AddPoint(points, base_lat, base_lng + 0.010 - (i * 0.001));
    }

    const auto detailed = resolver.Resolve(points);

    EXPECT_FALSE(detailed.boundary_hexes.empty());
    EXPECT_FALSE(detailed.enclosed_hexes.empty());
    EXPECT_FALSE(detailed.claimed_hexes.empty());

    EXPECT_GT(detailed.total_distance_meters, 500.0);

    std::unordered_set<uint64_t> unique_check(detailed.claimed_hexes.begin(), detailed.claimed_hexes.end());
    EXPECT_EQ(unique_check.size(), detailed.claimed_hexes.size());
}

} // namespace run::domain::h3::testing
