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
                  double lon) const
    {
        auto* point = points.Add();
        point->set_latitude(lat);
        point->set_longitude(lon);
    }
};

TEST_F(H3CaptureResolverTest, Resolve_EmptyPoints_ReturnsEmptyResult)
{
    H3CaptureResolver resolver(m_h3_service, kDefaultMinPoints);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    const auto total_claimed = resolver.Resolve(points);

    EXPECT_TRUE(total_claimed.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_NoiseFiltering_DropsUnderThreshold)
{
    H3CaptureResolver resolver(m_h3_service, 3);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    AddPoint(points, kBaseLat, kBaseLng);
    AddPoint(points, kBaseLat + 0.00001, kBaseLng + 0.00001);

    const auto total_claimed = resolver.Resolve(points);

    EXPECT_TRUE(total_claimed.empty());
}

TEST_F(H3CaptureResolverTest, Resolve_SimpleClosedLoop_ClaimsBoundaryAndEnclosed)
{
    H3CaptureResolver resolver(m_h3_service, kDefaultMinPoints);

    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat + (i * 0.001), kBaseLng);
    }
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat + 0.004, kBaseLng + (i * 0.001));
    }
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat + 0.004 - (i * 0.001), kBaseLng + 0.004);
    }
    for (int i = 0; i < 5; ++i)
    {
        AddPoint(points, kBaseLat, kBaseLng + 0.004 - (i * 0.001));
    }

    const auto total_claimed = resolver.Resolve(points);

    ASSERT_FALSE(total_claimed.empty());

    std::vector<H3IndexValue> unique_check = total_claimed;
    std::sort(unique_check.begin(), unique_check.end());
    auto last = std::unique(unique_check.begin(), unique_check.end());

    EXPECT_EQ(last, unique_check.end()) << "Result contains duplicate H3 cells!";

    for (auto cell : total_claimed)
    {
        EXPECT_NE(cell, 0);
        EXPECT_TRUE(H3Service::IsValid(cell));
    }
}

} // namespace run::domain::h3::testing
