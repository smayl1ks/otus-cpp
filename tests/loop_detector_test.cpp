#include "gameplay/territory_conquest/loop_detector.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace run::gameplay::territory_conquest::testing
{

class LoopDetectorTest : public ::testing::Test
{
protected:
    H3Service m_h3_service{H3Config{.default_resolution = 9}};
};

TEST_F(LoopDetectorTest, DetectLoops_ShortPerimeter_ReturnsNoLoops)
{
    LoopDetector detector(m_h3_service, LoopDetectorConfig{.min_loop_perimeter_meters = 300.0});

    // Small square with total perimeter ~40m
    const double base_lat = 55.7983;
    const double base_lon = 49.1061;
    const auto hex = *m_h3_service.PointToH3(base_lat, base_lon);

    std::vector<TrackPoint> track = {
        {.hex = hex, .coord = {base_lat, base_lon}, .cum_distance = 0.0},
        {.hex = hex, .coord = {base_lat, base_lon + 0.0001}, .cum_distance = 10.0},
        {.hex = hex, .coord = {base_lat + 0.0001, base_lon + 0.0001}, .cum_distance = 20.0},
        {.hex = hex, .coord = {base_lat + 0.0001, base_lon}, .cum_distance = 30.0},
        {.hex = hex, .coord = {base_lat, base_lon}, .cum_distance = 40.0},
        {.hex = hex, .coord = {base_lat, base_lon}, .cum_distance = 41.0},
    };

    auto loops = detector.DetectLoops(track);
    EXPECT_TRUE(loops.empty());
}

TEST_F(LoopDetectorTest, DetectLoops_ValidLoop_DetectsCorrectInterval)
{
    LoopDetector detector(m_h3_service, LoopDetectorConfig{.min_loop_perimeter_meters = 300.0,
                                                           .min_loop_area_m2 = 5000.0,
                                                           .max_loop_closure_distance_meters = 150.0});

    const double base_lat = 55.7983;
    const double base_lon = 49.1061;
    const double d = 0.003; // ~330m

    std::vector<TrackPoint> track;
    double cum_dist = 0.0;

    auto add_pt = [&](double lat, double lon)
    {
        if (!track.empty())
        {
            cum_dist +=
                H3Service::GetDistanceMeters(track.back().coord.latitude, track.back().coord.longitude, lat, lon);
        }
        track.push_back(
            TrackPoint{.hex = *m_h3_service.PointToH3(lat, lon), .coord = {lat, lon}, .cum_distance = cum_dist});
    };

    // 4 corners of a ~330m x 330m square
    add_pt(base_lat, base_lon);
    add_pt(base_lat, base_lon + d);
    add_pt(base_lat + d, base_lon + d);
    add_pt(base_lat + d, base_lon);
    add_pt(base_lat, base_lon);
    add_pt(base_lat, base_lon);

    auto loops = detector.DetectLoops(track);
    ASSERT_EQ(loops.size(), 1);
    EXPECT_EQ(loops[0].start_idx, 0);
    EXPECT_EQ(loops[0].end_idx, 4);
    EXPECT_GT(loops[0].signed_area, 5000.0);
}

} // namespace run::gameplay::territory_conquest::testing
