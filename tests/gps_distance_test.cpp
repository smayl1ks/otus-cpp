#include "gameplay/geo_point.hpp"
#include <gtest/gtest.h>

namespace run::gameplay::testing
{

TEST(GpsDistanceTest, CalculateDistance_SamePoints_ReturnsZero)
{
    const GeoPoint p{55.7983, 49.1061};
    EXPECT_DOUBLE_EQ(CalculateGCircleDistance(p, p), 0.0);
}

TEST(GpsDistanceTest, CalculateDistance_KnownDistance_Accurate)
{
    // Approximately 1 degree latitude is ~111.139 km
    const GeoPoint p1{55.0, 49.0};
    const GeoPoint p2{56.0, 49.0};

    const double dist = CalculateGCircleDistance(p1, p2);
    EXPECT_NEAR(dist, 111195.0, 100.0); // Within 100m of expected
}

TEST(GpsDistanceTest, CalculateDistance_Symmetric)
{
    const GeoPoint p1{55.7983, 49.1061};
    const GeoPoint p2{55.8034, 49.1442};

    EXPECT_DOUBLE_EQ(CalculateGCircleDistance(p1, p2), CalculateGCircleDistance(p2, p1));
}

} // namespace run::gameplay::testing
