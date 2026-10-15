#include "gameplay/territory_conquest/polygon_geometry.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace run::gameplay::territory_conquest::testing
{

TEST(PolygonGeometryTest, CalculateSignedArea_LessThanThreeVertices_ReturnsZero)
{
    std::vector<GeoPoint> pts = {{55.0, 49.0}, {55.01, 49.01}};
    EXPECT_DOUBLE_EQ(PolygonGeometry::CalculateSignedAreaM2(pts), 0.0);
}

TEST(PolygonGeometryTest, CalculateSignedArea_CCW_ReturnsPositiveArea)
{
    // ~100m x ~100m square counter-clockwise in Kazan
    const double base_lat = 55.7983;
    const double base_lon = 49.1061;
    const double d_lat = 0.001;  // ~111m
    const double d_lon = 0.0016; // ~100m at lat 55.8

    std::vector<GeoPoint> ccw_square = {{base_lat, base_lon},
                                        {base_lat, base_lon + d_lon},
                                        {base_lat + d_lat, base_lon + d_lon},
                                        {base_lat + d_lat, base_lon}};

    const double area = PolygonGeometry::CalculateSignedAreaM2(ccw_square);
    EXPECT_GT(area, 0.0);
    EXPECT_NEAR(area, 11100.0, 2000.0);
}

TEST(PolygonGeometryTest, CalculateSignedArea_CW_ReturnsNegativeArea)
{
    const double base_lat = 55.7983;
    const double base_lon = 49.1061;
    const double d_lat = 0.001;
    const double d_lon = 0.0016;

    std::vector<GeoPoint> cw_square = {{base_lat, base_lon},
                                       {base_lat + d_lat, base_lon},
                                       {base_lat + d_lat, base_lon + d_lon},
                                       {base_lat, base_lon + d_lon}};

    const double area = PolygonGeometry::CalculateSignedAreaM2(cw_square);
    EXPECT_LT(area, 0.0);
}

TEST(PolygonGeometryTest, BuildNormalizedH3Loop_ReversesNegativeAreaToCCW)
{
    const double base_lat = 55.7983;
    const double base_lon = 49.1061;

    std::vector<GeoPoint> cw_square = {{base_lat, base_lon},
                                       {base_lat + 0.001, base_lon},
                                       {base_lat + 0.001, base_lon + 0.001},
                                       {base_lat, base_lon + 0.001}};

    auto loop = PolygonGeometry::BuildNormalizedH3Loop(cw_square, -5000.0);
    ASSERT_EQ(loop.size(), 4);
    // First vertex of reversed loop should match original last vertex
    EXPECT_NEAR(loop.front().lat, (base_lat * std::numbers::pi / 180.0), 1e-6);
}

TEST(PolygonGeometryTest, BuildNormalizedH3Loop_DeduplicatesAdjacentVertices)
{
    const double base_lat = 55.7983;
    const double base_lon = 49.1061;

    std::vector<GeoPoint> pts_with_dups = {
        {base_lat, base_lon},         {base_lat, base_lon}, // duplicate
        {base_lat + 0.001, base_lon}, {base_lat + 0.001, base_lon + 0.001},
        {base_lat, base_lon + 0.001}, {base_lat, base_lon} // closing duplicate
    };

    auto loop = PolygonGeometry::BuildNormalizedH3Loop(pts_with_dups, 5000.0);
    EXPECT_EQ(loop.size(), 4);
}

} // namespace run::gameplay::territory_conquest::testing
