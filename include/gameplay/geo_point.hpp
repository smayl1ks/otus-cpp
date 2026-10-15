#pragma once

namespace run::gameplay
{

/**
 * @brief Geographic coordinate (WGS84) in degrees.
 */
struct GeoPoint
{
    double latitude{0.0};  ///< [-90.0, 90.0]
    double longitude{0.0}; ///< [-180.0, 180.0]

    constexpr bool operator==(const GeoPoint&) const noexcept = default;
};

/**
 * @brief Calculates great-circle distance between two geographic coordinates
 *        using Haversine formula on spherical Earth.
 */
double CalculateGCircleDistance(double lat1, double lon1, double lat2, double lon2) noexcept;

double CalculateGCircleDistance(const GeoPoint& p1, const GeoPoint& p2) noexcept;

} // namespace run::gameplay
