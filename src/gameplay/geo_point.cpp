#include "gameplay/geo_point.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace run::gameplay
{

namespace
{
constexpr double g_EarthRadiusMeters = 6371000.0; // WGS84 mean radius
constexpr double g_DegToRad = std::numbers::pi / 180.0;
} // namespace

double CalculateGCircleDistance(double lat1, double lon1, double lat2, double lon2) noexcept
{
    const double d_lat = (lat2 - lat1) * g_DegToRad;
    const double d_lon = (lon2 - lon1) * g_DegToRad;

    const double a = std::sin(d_lat / 2.0) * std::sin(d_lat / 2.0) + std::cos(lat1 * g_DegToRad) *
                                                                         std::cos(lat2 * g_DegToRad) *
                                                                         std::sin(d_lon / 2.0) * std::sin(d_lon / 2.0);

    // Clamp against numerical precision issues that could cause NaN in sqrt(1 - a)
    const double clamped_a = std::clamp(a, 0.0, 1.0);
    const double c = 2.0 * std::atan2(std::sqrt(clamped_a), std::sqrt(1.0 - clamped_a));

    return g_EarthRadiusMeters * c;
}

double CalculateGCircleDistance(const GeoPoint& p1, const GeoPoint& p2) noexcept
{
    return CalculateGCircleDistance(p1.latitude, p1.longitude, p2.latitude, p2.longitude);
}

} // namespace run::gameplay
