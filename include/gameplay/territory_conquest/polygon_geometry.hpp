#pragma once

#include "gameplay/geo_point.hpp"
#include <h3/h3api.h>
#include <span>
#include <vector>

namespace run::gameplay::territory_conquest
{

/**
 * @brief Utilities for spherical and planar polygon geometry calculations and normalization.
 */
class PolygonGeometry
{
public:
    /**
     * @brief Computes signed planar polygon area in square meters using the Shoelace formula
     *        under local equirectangular projection centered around mean latitude.
     * @return Positive area for Counter-Clockwise (CCW) vertex order, negative for Clockwise (CW).
     */
    static double CalculateSignedAreaM2(std::span<const GeoPoint> vertices) noexcept;

    /**
     * @brief Prepares and normalizes vertices for Uber H3 GeoPolygon:
     *        - Deduplicates adjacent identical coordinates within epsilon.
     *        - Removes redundant closing vertex if equal to the first vertex.
     *        - Normalizes vertex sequence to Counter-Clockwise (CCW) if signed_area is negative.
     */
    static std::vector<LatLng> BuildNormalizedH3Loop(std::span<const GeoPoint> vertices, double signed_area);
};

} // namespace run::gameplay::territory_conquest
