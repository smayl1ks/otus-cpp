#include "domain/h3_service.hpp"

#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <unordered_set>

namespace run::domain::h3::testing
{

class H3ServiceTest : public ::testing::Test
{
protected:
    H3Service m_service{H3Config{.default_resolution = 9}};

    const double kKazanLat = 55.7983;
    const double kKazanLng = 49.1061;

    // H3_NULL
    const H3IndexValue kInvalidH3Cell = 0;
};

TEST_F(H3ServiceTest, PointToH3_ValidCoordinates_ReturnsValidCell)
{
    const auto cell = m_service.PointToH3(kKazanLat, kKazanLng);

    ASSERT_TRUE(cell.has_value());
    EXPECT_NE(*cell, kInvalidH3Cell);
    EXPECT_TRUE(H3Service::IsValid(*cell));
}

TEST_F(H3ServiceTest, PointToH3_InvalidCoordinates_ReturnsNullopt)
{
    // latitude [-90, 90]
    EXPECT_FALSE(m_service.PointToH3(95.0, 49.1061).has_value());
    EXPECT_FALSE(m_service.PointToH3(-91.0, 49.1061).has_value());

    // longitude [-180, 180]
    EXPECT_FALSE(m_service.PointToH3(55.7983, 185.0).has_value());
    EXPECT_FALSE(m_service.PointToH3(55.7983, -181.0).has_value());
}

TEST_F(H3ServiceTest, PointToH3_ExplicitResolution_UsesSpecifiedResolution)
{
    const int target_res = 7;
    const auto cell_res7 = m_service.PointToH3(kKazanLat, kKazanLng, target_res);
    const auto cell_default = m_service.PointToH3(kKazanLat, kKazanLng);

    ASSERT_TRUE(cell_res7.has_value());
    ASSERT_TRUE(cell_default.has_value());

    EXPECT_NE(*cell_res7, *cell_default);
}

TEST_F(H3ServiceTest, H3ToPoint_ValidCell_ReturnsCoordinatesCloseToOriginal)
{
    const auto original_cell = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(original_cell.has_value());

    const auto coords = m_service.H3ToPoint(*original_cell);
    ASSERT_TRUE(coords.has_value());

    EXPECT_NEAR(coords->latitude, kKazanLat, 0.005);
    EXPECT_NEAR(coords->longitude, kKazanLng, 0.005);
}

TEST_F(H3ServiceTest, H3ToPoint_InvalidCell_ReturnsNullopt)
{
    EXPECT_FALSE(m_service.H3ToPoint(kInvalidH3Cell).has_value());
}

TEST_F(H3ServiceTest, GetHexagonsInRadius_ZeroRadius_ReturnsOnlyCenterCell)
{
    const auto center = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(center.has_value());

    const auto hexes = m_service.GetHexagonsInRadius(*center, 0);

    ASSERT_EQ(hexes.size(), 1u);
    EXPECT_EQ(hexes[0], *center);
}

TEST_F(H3ServiceTest, GetHexagonsInRadius_RadiusOne_ReturnsSevenUniqueCells)
{
    const auto center = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(center.has_value());

    const auto hexes = m_service.GetHexagonsInRadius(*center, 1);

    // K-ring r = 1 contains 1 + 6 = 7 elements
    EXPECT_EQ(hexes.size(), 7u);

    std::unordered_set<H3IndexValue> unique_hexes(hexes.begin(), hexes.end());
    EXPECT_EQ(unique_hexes.size(), 7u);
    EXPECT_EQ(unique_hexes.count(kInvalidH3Cell), 0u);
}

TEST_F(H3ServiceTest, GetHexagonsInRadius_InvalidCellOrNegativeRadius_ReturnsEmpty)
{
    const auto center = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(center.has_value());

    EXPECT_TRUE(m_service.GetHexagonsInRadius(kInvalidH3Cell, 1).empty());
    EXPECT_TRUE(m_service.GetHexagonsInRadius(*center, -1).empty());
}

TEST_F(H3ServiceTest, GetPathBetweenHexes_NeighborCells_ReturnsSequence)
{
    const auto start = m_service.PointToH3(kKazanLat, kKazanLng);

    const auto end = m_service.PointToH3(kKazanLat + 0.002, kKazanLng + 0.002);

    ASSERT_TRUE(start.has_value());
    ASSERT_TRUE(end.has_value());

    const auto path = m_service.GetPathBetweenHexes(*start, *end);

    ASSERT_FALSE(path.empty());
    EXPECT_EQ(path.front(), *start);
    EXPECT_EQ(path.back(), *end);
}

TEST_F(H3ServiceTest, GetCellsInBoundingBox_ValidBbox_ReturnsContainedCells)
{
    BoundingBox bbox{.south_west = {.latitude = 55.7900, .longitude = 49.1000},
                     .north_east = {.latitude = 55.8000, .longitude = 49.1100}};

    const auto cells = m_service.GetCellsInBoundingBox(bbox, 9);

    EXPECT_FALSE(cells.empty());
    for (const auto cell : cells)
    {
        EXPECT_TRUE(H3Service::IsValid(cell));
    }
}

TEST_F(H3ServiceTest, GetPathBetweenHexes_SameCell_ReturnsSingleCell)
{
    const auto cell = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(cell.has_value());

    const auto path = m_service.GetPathBetweenHexes(*cell, *cell);

    ASSERT_EQ(path.size(), 1u);
    EXPECT_EQ(path[0], *cell);
}

TEST_F(H3ServiceTest, GetPathBetweenHexes_InvalidInputs_ReturnsEmpty)
{
    const auto valid_cell = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(valid_cell.has_value());

    EXPECT_TRUE(m_service.GetPathBetweenHexes(kInvalidH3Cell, *valid_cell).empty());
    EXPECT_TRUE(m_service.GetPathBetweenHexes(*valid_cell, kInvalidH3Cell).empty());
}

TEST_F(H3ServiceTest, GetParentZones_DeduplicatesParentCells)
{
    const auto center = m_service.PointToH3(kKazanLat, kKazanLng, 9);
    ASSERT_TRUE(center.has_value());

    const auto child_cells = m_service.GetHexagonsInRadius(*center, 1);
    ASSERT_EQ(child_cells.size(), 7u);

    const auto parent_zones = m_service.GetParentZones(child_cells, 6);

    EXPECT_FALSE(parent_zones.empty());
    EXPECT_LE(parent_zones.size(), 2u);
}

TEST_F(H3ServiceTest, GetParent_ValidCellAndLowerResolution_ReturnsParentCell)
{
    const int child_res = 9;
    const int parent_res = 6;

    const auto child = m_service.PointToH3(kKazanLat, kKazanLng, child_res);
    ASSERT_TRUE(child.has_value());

    const auto parent = m_service.GetParent(*child, parent_res);

    ASSERT_TRUE(parent.has_value());
    EXPECT_TRUE(H3Service::IsValid(*parent));
    EXPECT_NE(*child, *parent);
}

TEST_F(H3ServiceTest, GetHexagonBoundary_ValidCell_ReturnsSixVertices)
{
    const auto cell = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(cell.has_value());

    const auto boundary = m_service.GetHexagonBoundary(*cell);

    EXPECT_EQ(boundary.size(), 6u);
}

TEST_F(H3ServiceTest, StringConversion_RoundTrip_PreservesValue)
{
    const auto cell = m_service.PointToH3(kKazanLat, kKazanLng);
    ASSERT_TRUE(cell.has_value());

    const std::string str_repr = H3Service::ToString(*cell);
    EXPECT_FALSE(str_repr.empty());

    const auto parsed_cell = H3Service::FromString(str_repr);
    ASSERT_TRUE(parsed_cell.has_value());
    EXPECT_EQ(*parsed_cell, *cell);
}

TEST_F(H3ServiceTest, FromString_InvalidString_ReturnsNullopt)
{
    EXPECT_FALSE(H3Service::FromString("not_a_valide_hex").has_value());
    EXPECT_FALSE(H3Service::FromString("").has_value());
}

TEST_F(H3ServiceTest, GetDistanceMeters_SameCoordinates_ReturnsZero)
{
    const double distance = H3Service::GetDistanceMeters(kKazanLat, kKazanLng, kKazanLat, kKazanLng);

    EXPECT_NEAR(distance, 0.0, 1e-6);
}

TEST_F(H3ServiceTest, GetDistanceMeters_KnownPoints_ReturnsAccurateDistance)
{
    const double stadium_lat = 55.7963;
    const double stadium_lng = 49.0975;

    const double distance = H3Service::GetDistanceMeters(kKazanLat, kKazanLng, stadium_lat, stadium_lng);

    EXPECT_NEAR(distance, 581.4, 5.0);
}

} // namespace run::domain::h3::testing