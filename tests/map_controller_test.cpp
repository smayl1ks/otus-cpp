#include <gtest/gtest.h>

#include "controllers/map_controller.hpp"

#include "gamemap.pb.h"

#include <cstdint>

namespace
{

using run::controller::IsValidViewportBBox;
using run::controller::kMaxSubscribedZones;
using run::controller::NormalizeZones;

gamemap::SubscribeViewportRequest MakeBBox(double sw_lat, double sw_lng, double ne_lat, double ne_lng)
{
    gamemap::SubscribeViewportRequest request;
    request.set_south_west_lat(sw_lat);
    request.set_south_west_lng(sw_lng);
    request.set_north_east_lat(ne_lat);
    request.set_north_east_lng(ne_lng);
    return request;
}

TEST(ViewportBBox, AcceptsTypicalCityCamera)
{
    EXPECT_TRUE(IsValidViewportBBox(MakeBBox(55.70, 49.10, 55.85, 49.35)));
}

TEST(ViewportBBox, AcceptsExactSpanLimits)
{
    EXPECT_TRUE(IsValidViewportBBox(MakeBBox(55.0, 49.0, 57.0, 53.0))); // 2.0 lat, 4.0 lng
}

TEST(ViewportBBox, RejectsNanAndInf)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(nan, 49.0, 55.8, 49.3)));
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(55.7, inf, 55.8, 49.3)));
}

TEST(ViewportBBox, RejectsOutOfRangeCoordinates)
{
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(-91.0, 49.0, 55.8, 49.3)));
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(55.7, -180.5, 55.8, 49.3)));
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(55.7, 49.0, 90.5, 49.3)));
}

TEST(ViewportBBox, RejectsInvertedCorners)
{
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(55.8, 49.0, 55.7, 49.3)));
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(55.7, 49.4, 55.8, 49.3)));
}

TEST(ViewportBBox, RejectsOverspan)
{
    // The N1 explosion guard: a world-wide bbox must not pass.
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(-90.0, -180.0, 90.0, 180.0)));
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(55.0, 49.0, 58.0, 53.0))); // 3 deg lat
    EXPECT_FALSE(IsValidViewportBBox(MakeBBox(55.0, 49.0, 57.0, 54.0))); // 5 deg lng
}

TEST(NormalizeZones, DropsZeroesSortsAndDeduplicates)
{
    gamemap::SubscribeViewportRequest request;
    request.add_zone_h3(300);
    request.add_zone_h3(0);
    request.add_zone_h3(100);
    request.add_zone_h3(100);

    const auto zones = NormalizeZones(request.zone_h3());
    EXPECT_EQ(zones, (std::vector<uint64_t>{100, 300}));
}

TEST(NormalizeZones, CapsAtMaxSubscribedZones)
{
    gamemap::SubscribeViewportRequest request;
    for (uint64_t i = 1; i <= kMaxSubscribedZones + 50; ++i)
    {
        request.add_zone_h3(i);
    }

    const auto zones = NormalizeZones(request.zone_h3());
    EXPECT_EQ(zones.size(), kMaxSubscribedZones);
    EXPECT_EQ(zones.back(), kMaxSubscribedZones); // smallest kept, tail cut
}

} // namespace
