#pragma once

#include <h3/h3api.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace run::domain::h3
{

using H3IndexValue = uint64_t;

struct GeoCoordinates
{
    double latitude{0.0};  ///< Latitude in degrees [-90, 90]
    double longitude{0.0}; ///< Longitude in degrees [-180, 180]
};

struct BoundingBox
{
    GeoCoordinates south_west;
    GeoCoordinates north_east;
};

struct H3Config
{
    int default_resolution{9}; ///< H3 resolution level (Res 9 ~100m edge length)
};

class H3Service
{
public:
    explicit H3Service(H3Config config = {}) noexcept;

    std::optional<H3IndexValue> PointToH3(double latitude, double longitude, int resolution) const noexcept;
    std::optional<H3IndexValue> PointToH3(double latitude, double longitude) const noexcept;

    std::optional<GeoCoordinates> H3ToPoint(H3IndexValue cell) const noexcept;

    std::vector<H3IndexValue> GetHexagonsInRadius(H3IndexValue center_cell, int k_ring_radius) const noexcept;

    std::vector<H3IndexValue> GetPathBetweenHexes(H3IndexValue start_cell, H3IndexValue end_cell) const noexcept;

    std::vector<H3IndexValue> GetCellsInBoundingBox(const BoundingBox& bbox, int resolution) const noexcept;

    std::vector<H3IndexValue> GetCellsInBoundingBox(double south_west_lat, double south_west_lng, double north_east_lat,
                                                    double north_east_lng, int resolution) const noexcept;

    std::optional<H3IndexValue> GetParent(H3IndexValue cell, int parent_resolution) const noexcept;
    std::vector<H3IndexValue> GetParentZones(const std::vector<H3IndexValue>& cells,
                                             int parent_resolution) const noexcept;

    std::vector<GeoCoordinates> GetHexagonBoundary(H3IndexValue cell) const noexcept;
    static double GetDistanceMeters(double lat1, double lng1, double lat2, double lng2) noexcept;

    static std::string ToString(H3IndexValue cell);
    static std::optional<H3IndexValue> FromString(const std::string& h3_str) noexcept;
    static bool IsValid(H3IndexValue cell) noexcept;

    int GetDefaultResolution() const noexcept
    {
        return m_config.default_resolution;
    }

private:
    H3Config m_config;
};

} // namespace run::domain::h3