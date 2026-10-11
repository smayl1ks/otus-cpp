#include "domain/capture_processor.hpp"
#include "domain/h3_service.hpp"
#include "location.pb.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace run::domain::h3::testing
{

class CaptureProcessorTest : public ::testing::Test
{
protected:
    H3Service m_h3_service{H3Config{.default_resolution = 9}};

    CaptureProcessor m_capture_processor{m_h3_service, 2};

    const double kKazanLat = 55.7983;
    const double kKazanLng = 49.1061;

    void AddPoint(google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points, double lat,
                  double lng, int64_t timestamp_ms)
    {
        auto* point = points.Add();
        point->set_latitude(lat);
        point->set_longitude(lng);
        point->set_timestamp(timestamp_ms);
    }
};

TEST_F(CaptureProcessorTest, InterpolateTrack_EmptyInput_ReturnsEmptyVector)
{
    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;
    EXPECT_TRUE(m_capture_processor.ProcessRun(points).empty());
}

TEST_F(CaptureProcessorTest, InterpolateTrack_SinglePointPerHex_IsFilteredOut)
{
    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    AddPoint(points, kKazanLat, kKazanLng, 1000);

    const auto result = m_capture_processor.InterpolateTrack(points);

    EXPECT_TRUE(result.empty());
}

TEST_F(CaptureProcessorTest, InterpolateTrack_TwoPointsInSameHex_IsQualified)
{
    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    AddPoint(points, kKazanLat, kKazanLng, 1000);
    AddPoint(points, kKazanLat + 0.00001, kKazanLng + 0.00001, 2000);

    const auto result = m_interpolator.InterpolateTrack(points);

    ASSERT_EQ(result.size(), 1u);

    const auto expected_h3 = m_h3_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(expected_h3.has_value());
    EXPECT_EQ(result[0], *expected_h3);
}

TEST_F(CaptureProcessorTest, InterpolateTrack_InvalidCoordinates_AreIgnored)
{
    google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint> points;

    // (Lat > 90)
    AddPoint(points, 120.0, 49.1061, 1000);
    AddPoint(points, 120.0, 49.1061, 2000);

    EXPECT_TRUE(m_interpolator.InterpolateTrack(points).empty());
}

} // namespace run::domain::h3::testing