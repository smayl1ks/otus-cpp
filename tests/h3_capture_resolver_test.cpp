#include "gameplay/territory_conquest/capture_resolver.hpp"
#include "gameplay/territory_conquest/h3_service.hpp"
#include "gameplay/workout_tracking/workout_session.hpp"
#include <algorithm>
#include <gtest/gtest.h>
#include <vector>

namespace run::gameplay::territory_conquest::testing
{

class H3CaptureResolverTest : public ::testing::Test
{
protected:
    H3Service m_h3_service{H3Config{.default_resolution = 9}};

    const std::size_t kDefaultMinPoints = 1;

    const double kBaseLat = 55.7983;
    const double kBaseLng = 49.1061;

    void AddPoint(std::vector<GeoPoint>& points, double lat, double lon) const
    {
        points.push_back(GeoPoint{.latitude = lat, .longitude = lon});
    }
};

TEST_F(H3CaptureResolverTest, Resolve_EmptyPoints_ReturnsEmptyResult)
{
    H3CaptureResolver resolver(m_h3_service);

    std::vector<GeoPoint> points;

    const auto result = resolver.Resolve(points);

    EXPECT_TRUE(result.claimed_hexes.empty());
    EXPECT_TRUE(result.boundary_hexes.empty());
    EXPECT_TRUE(result.enclosed_hexes.empty());
    EXPECT_DOUBLE_EQ(result.total_distance_meters, 0.0);
}

TEST_F(H3CaptureResolverTest, Resolve_MinDistanceFiltering_DropsHexBelowDistanceThreshold)
{
    // Points satisfy min_points_per_hex (2 points), but distance is only ~3 meters (below 15m threshold)
    H3CaptureConfig config{.min_points_per_hex = 2, .min_distance_per_hex_meters = 15.0};
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> points;
    AddPoint(points, kBaseLat, kBaseLng);
    AddPoint(points, kBaseLat + 0.00003, kBaseLng); // ~3.3m away in same hex

    const auto result = resolver.Resolve(points);

    EXPECT_TRUE(result.boundary_hexes.empty());
    EXPECT_TRUE(result.claimed_hexes.empty());
    EXPECT_GT(result.total_distance_meters, 0.0);
}

TEST_F(H3CaptureResolverTest, Resolve_MinPointsFiltering_DropsHexWithSinglePoint)
{
    H3CaptureConfig config{.min_points_per_hex = 2, .min_distance_per_hex_meters = 0.0};
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> points;
    AddPoint(points, kBaseLat, kBaseLng); // Only 1 point in hex

    const auto result = resolver.Resolve(points);

    EXPECT_TRUE(result.boundary_hexes.empty());
    EXPECT_TRUE(result.claimed_hexes.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_SingleHexLinearTrack_CapturesBoundaryHex)
{
    H3CaptureConfig config{.min_points_per_hex = 1, .min_distance_per_hex_meters = 0.0};
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> points;
    AddPoint(points, kBaseLat, kBaseLng);
    AddPoint(points, kBaseLat + 0.0001, kBaseLng);

    const auto result = resolver.Resolve(points);

    const auto expected_hex = m_h3_service.PointToH3(kBaseLat, kBaseLng).value_or(0);
    EXPECT_EQ(result.boundary_hexes.size(), 1u);
    EXPECT_EQ(result.boundary_hexes[0], expected_hex);
    EXPECT_EQ(result.claimed_hexes.size(), 1u);
    EXPECT_EQ(result.claimed_hexes[0], expected_hex);
    EXPECT_TRUE(result.enclosed_hexes.empty());
    EXPECT_GT(result.total_distance_meters, 0.0);
}

TEST_F(H3CaptureResolverTest, Resolve_OpenTrajectory_OnlyBoundaryHexesCaptured)
{
    H3CaptureConfig config{.min_points_per_hex = 1, .min_distance_per_hex_meters = 0.0};
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> points;
    // Walk in a straight line across multiple hexes
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat + (i * 0.002), kBaseLng);
    }

    const auto result = resolver.Resolve(points);

    EXPECT_GT(result.boundary_hexes.size(), 1u);
    EXPECT_TRUE(result.enclosed_hexes.empty());
    EXPECT_EQ(result.claimed_hexes.size(), result.boundary_hexes.size());

    // Claimed hexes should be sorted and deduplicated
    EXPECT_TRUE(std::is_sorted(result.claimed_hexes.begin(), result.claimed_hexes.end()));
}

TEST_F(H3CaptureResolverTest, Resolve_ClosedLoop_RasterizesAndEnclosesInteriorHexes)
{
    // Configure loop capture: min perimeter 300m, min area 10,000 m2
    H3CaptureConfig config{
        .min_points_per_hex = 1,
        .min_distance_per_hex_meters = 0.0,
        .enable_loop_capture = true,
        .min_loop_perimeter_meters = 300.0,
        .min_loop_area_m2 = 10000.0,
        .max_loop_closure_distance_meters = 150.0,
    };
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> points;

    // Draw a large square (~800m x 800m, area ~640,000 m2 > 10,000 m2 threshold)
    // 0.008 deg latitude ~ 890m, 0.008 deg longitude at 55N ~ 510m
    const double step = 0.002;
    // Bottom edge: west to east
    for (int i = 0; i < 4; ++i)
    {
        AddPoint(points, kBaseLat, kBaseLng + (i * step));
    }
    // Right edge: south to north
    for (int i = 0; i < 4; ++i)
    {
        AddPoint(points, kBaseLat + (i * step), kBaseLng + 0.008);
    }
    // Top edge: east to west
    for (int i = 0; i < 4; ++i)
    {
        AddPoint(points, kBaseLat + 0.008, kBaseLng + 0.008 - (i * step));
    }
    // Left edge: north to south, closing back near origin
    for (int i = 0; i < 4; ++i)
    {
        AddPoint(points, kBaseLat + 0.008 - (i * step), kBaseLng);
    }
    // Close the loop explicitly
    AddPoint(points, kBaseLat, kBaseLng);

    const auto result = resolver.Resolve(points);

    EXPECT_FALSE(result.boundary_hexes.empty());
    EXPECT_FALSE(result.enclosed_hexes.empty());

    // Claimed hexes should contain all boundary hexes plus enclosed interior hexes
    EXPECT_GE(result.claimed_hexes.size(), result.boundary_hexes.size());
    EXPECT_GE(result.claimed_hexes.size(), result.enclosed_hexes.size());

    // Claimed hexes must be strictly deduplicated and sorted
    EXPECT_TRUE(std::is_sorted(result.claimed_hexes.begin(), result.claimed_hexes.end()));
    EXPECT_TRUE(std::adjacent_find(result.claimed_hexes.begin(), result.claimed_hexes.end()) ==
                result.claimed_hexes.end());
}

TEST_F(H3CaptureResolverTest, Resolve_LoopDisabled_NoEnclosedHexes)
{
    H3CaptureConfig config{
        .min_points_per_hex = 1,
        .min_distance_per_hex_meters = 0.0,
        .enable_loop_capture = false, // Loop capture explicitly disabled
    };
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> points;
    // Square loop
    AddPoint(points, kBaseLat, kBaseLng);
    AddPoint(points, kBaseLat, kBaseLng + 0.008);
    AddPoint(points, kBaseLat + 0.008, kBaseLng + 0.008);
    AddPoint(points, kBaseLat + 0.008, kBaseLng);
    AddPoint(points, kBaseLat, kBaseLng);

    const auto result = resolver.Resolve(points);

    EXPECT_FALSE(result.boundary_hexes.empty());
    EXPECT_TRUE(result.enclosed_hexes.empty());
    EXPECT_EQ(result.claimed_hexes.size(), result.boundary_hexes.size());
}

TEST_F(H3CaptureResolverTest, Resolve_SmallLoopBelowAreaThreshold_RejectsEnclosedCapture)
{
    // Require minimum 50,000 m2
    H3CaptureConfig config{
        .min_points_per_hex = 1,
        .min_distance_per_hex_meters = 0.0,
        .enable_loop_capture = true,
        .min_loop_perimeter_meters = 50.0,
        .min_loop_area_m2 = 50000.0, // High area threshold
        .max_loop_closure_distance_meters = 150.0,
    };
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> points;
    // Small tiny loop ~20m x 20m (area ~400 m2 << 50,000 m2)
    AddPoint(points, kBaseLat, kBaseLng);
    AddPoint(points, kBaseLat, kBaseLng + 0.0002);
    AddPoint(points, kBaseLat + 0.0002, kBaseLng + 0.0002);
    AddPoint(points, kBaseLat + 0.0002, kBaseLng);
    AddPoint(points, kBaseLat, kBaseLng);

    const auto result = resolver.Resolve(points);

    // Loop detected, but area is below 50,000 m2, so enclosed hexes must be empty
    EXPECT_TRUE(result.enclosed_hexes.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_RunPointSpan_MatchesGeoPointResult)
{
    H3CaptureConfig config{.min_points_per_hex = 1, .min_distance_per_hex_meters = 0.0};
    H3CaptureResolver resolver(m_h3_service, config);

    std::vector<GeoPoint> geo_points;
    std::vector<::run::gameplay::workout_tracking::RunPoint> run_points;

    for (int i = 0; i < 4; ++i)
    {
        double lat = kBaseLat + (i * 0.002);
        double lon = kBaseLng;
        geo_points.push_back(GeoPoint{.latitude = lat, .longitude = lon});
        run_points.push_back(::run::gameplay::workout_tracking::RunPoint::Create(1, i, lat, lon, 1000 * i));
    }

    const auto result_geo = resolver.Resolve(geo_points);
    const auto result_run = resolver.Resolve(run_points);

    EXPECT_EQ(result_geo.claimed_hexes, result_run.claimed_hexes);
    EXPECT_EQ(result_geo.boundary_hexes, result_run.boundary_hexes);
    EXPECT_EQ(result_geo.enclosed_hexes, result_run.enclosed_hexes);
    EXPECT_DOUBLE_EQ(result_geo.total_distance_meters, result_run.total_distance_meters);
}

} // namespace run::gameplay::territory_conquest::testing
