#include "gameplay/territory_conquest/capture_resolver.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace run::gameplay::territory_conquest::testing
{

TEST(BoundaryQualifierTest, Qualify_EmptyTrack_ReturnsEmpty)
{
    std::vector<TrackPoint> track;
    auto qualified = BoundaryQualifier::QualifyBoundaryHexes(track, 2, 15.0);
    EXPECT_TRUE(qualified.empty());
}

TEST(BoundaryQualifierTest, Qualify_PreservesChronologicalOrder)
{
    // Runner visits hex 100, then 200, then 300
    std::vector<TrackPoint> track = {
        TrackPoint{.hex = 100, .coord = {55.000, 49.000}, .cum_distance = 0.0},
        TrackPoint{.hex = 100, .coord = {55.001, 49.000}, .cum_distance = 111.0},
        TrackPoint{.hex = 200, .coord = {55.002, 49.000}, .cum_distance = 222.0},
        TrackPoint{.hex = 200, .coord = {55.003, 49.000}, .cum_distance = 333.0},
        TrackPoint{.hex = 300, .coord = {55.004, 49.000}, .cum_distance = 444.0},
        TrackPoint{.hex = 300, .coord = {55.005, 49.000}, .cum_distance = 555.0},
    };

    auto qualified = BoundaryQualifier::QualifyBoundaryHexes(track, 2, 10.0);
    ASSERT_EQ(qualified.size(), 3);
    EXPECT_EQ(qualified[0], 100);
    EXPECT_EQ(qualified[1], 200);
    EXPECT_EQ(qualified[2], 300);
}

TEST(BoundaryQualifierTest, Qualify_FiltersUnderMinPoints)
{
    // Hex 200 has only 1 point -> should be dropped when min_points is 2
    std::vector<TrackPoint> track = {
        TrackPoint{.hex = 100, .coord = {55.000, 49.000}, .cum_distance = 0.0},
        TrackPoint{.hex = 100, .coord = {55.001, 49.000}, .cum_distance = 111.0},
        TrackPoint{.hex = 200, .coord = {55.002, 49.000}, .cum_distance = 222.0},
    };

    auto qualified = BoundaryQualifier::QualifyBoundaryHexes(track, 2, 0.0);
    ASSERT_EQ(qualified.size(), 1);
    EXPECT_EQ(qualified[0], 100);
}

} // namespace run::gameplay::territory_conquest::testing
