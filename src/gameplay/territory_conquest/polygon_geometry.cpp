#include "gameplay/territory_conquest/polygon_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace run::gameplay::territory_conquest
{

namespace
{
constexpr double g_DegToRad = std::numbers::pi / 180.0;
constexpr double g_MetersPerDegreeLat = 111139.0;
constexpr double g_EpsilonDegrees = 1e-7;
} // namespace

double PolygonGeometry::CalculateSignedAreaM2(std::span<const GeoPoint> vertices) noexcept
{
    if (vertices.size() < 3)
    {
        return 0.0;
    }

    double mean_lat = 0.0;
    for (const auto& v : vertices)
    {
        mean_lat += v.latitude;
    }
    mean_lat /= static_cast<double>(vertices.size());

    const double meters_per_deg_lon = g_MetersPerDegreeLat * std::cos(mean_lat * g_DegToRad);

    double area = 0.0;
    const std::size_t n = vertices.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& p1 = vertices[i];
        const auto& p2 = vertices[(i + 1) % n];

        const double x1 = p1.longitude * meters_per_deg_lon;
        const double y1 = p1.latitude * g_MetersPerDegreeLat;
        const double x2 = p2.longitude * meters_per_deg_lon;
        const double y2 = p2.latitude * g_MetersPerDegreeLat;

        area += (x1 * y2) - (x2 * y1);
    }

    return 0.5 * area;
}

std::vector<LatLng> PolygonGeometry::BuildNormalizedH3Loop(std::span<const GeoPoint> vertices, double signed_area)
{
    if (vertices.empty())
    {
        return {};
    }

    std::vector<LatLng> result;
    result.reserve(vertices.size());

    for (const auto& pt : vertices)
    {
        const LatLng current{degsToRads(pt.latitude), degsToRads(pt.longitude)};
        if (!result.empty())
        {
            const auto& prev = result.back();
            if (std::abs(prev.lat - current.lat) < g_EpsilonDegrees &&
                std::abs(prev.lng - current.lng) < g_EpsilonDegrees)
            {
                continue; // Skip consecutive duplicate
            }
        }
        result.push_back(current);
    }

    // Remove redundant closing vertex if it matches the first vertex
    if (result.size() > 1)
    {
        const auto& first = result.front();
        const auto& last = result.back();
        if (std::abs(first.lat - last.lat) < g_EpsilonDegrees && std::abs(first.lng - last.lng) < g_EpsilonDegrees)
        {
            result.pop_back();
        }
    }

    // Invert clockwise loops to counter-clockwise for H3 compatibility
    if (signed_area < 0.0)
    {
        std::reverse(result.begin(), result.end());
    }

    return result;
}

} // namespace run::gameplay::territory_conquest
