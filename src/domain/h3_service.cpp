#include "domain/h3_service.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace run::domain::h3
{

H3Service::H3Service(H3Config config) noexcept : m_config(config)
{
}

bool H3Service::IsValid(H3IndexValue cell) noexcept
{
    return isValidCell(cell) != 0; // isValidCell provided by <h3/h3api.h>
}

std::optional<H3IndexValue> H3Service::PointToH3(double latitude, double longitude, int resolution) const noexcept
{
    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0)
    {
        return std::nullopt;
    }

    LatLng location;
    location.lat = degsToRads(latitude);
    location.lng = degsToRads(longitude);

    H3Index cell = H3_NULL;
    if (latLngToCell(&location, resolution, &cell) != E_SUCCESS || cell == H3_NULL)
    {
        return std::nullopt;
    }

    return cell;
}

std::optional<H3IndexValue> H3Service::PointToH3(double latitude, double longitude) const noexcept
{
    return PointToH3(latitude, longitude, m_config.default_resolution);
}

std::optional<GeoCoordinates> H3Service::H3ToPoint(H3IndexValue cell) const noexcept
{
    if (!IsValid(cell))
    {
        return std::nullopt;
    }

    LatLng location{};
    if (cellToLatLng(cell, &location) != E_SUCCESS)
    {
        return std::nullopt;
    }

    return GeoCoordinates{.latitude = radsToDegs(location.lat), .longitude = radsToDegs(location.lng)};
}

std::vector<H3IndexValue> H3Service::GetHexagonsInRadius(H3IndexValue center_cell, int k_ring_radius) const noexcept
{
    if (!IsValid(center_cell) || k_ring_radius < 0)
    {
        return {};
    }

    int64_t max_cells = 0;
    if (maxGridDiskSize(k_ring_radius, &max_cells) != E_SUCCESS || max_cells <= 0)
    {
        return {};
    }

    std::vector<H3IndexValue> result(static_cast<std::size_t>(max_cells), H3_NULL);
    if (gridDisk(center_cell, k_ring_radius, result.data()) != E_SUCCESS)
    {
        return {};
    }

    result.erase(std::remove(result.begin(), result.end(), H3_NULL), result.end());
    return result;
}

std::vector<H3IndexValue> H3Service::GetPathBetweenHexes(H3IndexValue start_cell, H3IndexValue end_cell) const noexcept
{
    if (!IsValid(start_cell) || !IsValid(end_cell))
    {
        return {};
    }

    int64_t path_size = 0;
    if (gridPathCellsSize(start_cell, end_cell, &path_size) != E_SUCCESS || path_size <= 0)
    {
        return {};
    }

    std::vector<H3IndexValue> path(static_cast<std::size_t>(path_size), H3_NULL);
    if (gridPathCells(start_cell, end_cell, path.data()) != E_SUCCESS)
    {
        return {};
    }

    path.erase(std::remove(path.begin(), path.end(), H3_NULL), path.end());
    return path;
}

std::vector<H3IndexValue> H3Service::GetCellsInBoundingBox(double south_west_lat, double south_west_lng,
                                                           double north_east_lat, double north_east_lng,
                                                           int resolution) const noexcept
{
    LatLng verts[4];

    // South-West
    verts[0].lat = degsToRads(south_west_lat);
    verts[0].lng = degsToRads(south_west_lng);

    // South-East
    verts[1].lat = degsToRads(south_west_lat);
    verts[1].lng = degsToRads(north_east_lng);

    // North-East
    verts[2].lat = degsToRads(north_east_lat);
    verts[2].lng = degsToRads(north_east_lng);

    // North-West
    verts[3].lat = degsToRads(north_east_lat);
    verts[3].lng = degsToRads(south_west_lng);

    GeoLoop outer_loop;
    outer_loop.numVerts = 4;
    outer_loop.verts = verts;

    GeoPolygon polygon;
    polygon.geoloop = outer_loop;
    polygon.numHoles = 0;
    polygon.holes = nullptr;

    int64_t max_cells = 0;
    if (maxPolygonToCellsSize(&polygon, resolution, 0, &max_cells) != E_SUCCESS || max_cells <= 0)
    {
        return {};
    }

    std::vector<H3IndexValue> cells(static_cast<std::size_t>(max_cells), H3_NULL);
    if (polygonToCells(&polygon, resolution, 0, cells.data()) != E_SUCCESS)
    {
        return {};
    }

    cells.erase(std::remove(cells.begin(), cells.end(), H3_NULL), cells.end());
    return cells;
}

std::vector<H3IndexValue> H3Service::GetCellsInBoundingBox(const BoundingBox& bbox, int resolution) const noexcept
{
    return GetCellsInBoundingBox(bbox.south_west.latitude, bbox.south_west.longitude, bbox.north_east.latitude,
                                 bbox.north_east.longitude, resolution);
}

std::optional<H3IndexValue> H3Service::GetParent(H3IndexValue cell, int parent_resolution) const noexcept
{
    if (!IsValid(cell))
    {
        return std::nullopt;
    }

    H3Index parent = H3_NULL;
    if (cellToParent(cell, parent_resolution, &parent) != E_SUCCESS || parent == H3_NULL)
    {
        return std::nullopt;
    }

    return parent;
}

std::vector<H3IndexValue> H3Service::GetParentZones(const std::vector<H3IndexValue>& cells,
                                                    int parent_resolution) const noexcept
{
    std::unordered_set<H3IndexValue> unique_parents;
    unique_parents.reserve(cells.size() / 7 + 1);

    for (const H3IndexValue cell : cells)
    {
        const auto parent = GetParent(cell, parent_resolution);
        if (parent.has_value() && *parent != H3_NULL)
        {
            unique_parents.insert(*parent);
        }
    }

    return std::vector<H3IndexValue>(unique_parents.begin(), unique_parents.end());
}

double H3Service::GetDistanceMeters(double lat1, double lng1, double lat2, double lng2) noexcept
{
    const LatLng a{degsToRads(lat1), degsToRads(lng1)};
    const LatLng b{degsToRads(lat2), degsToRads(lng2)};
    return greatCircleDistanceM(&a, &b);
}

std::vector<GeoCoordinates> H3Service::GetHexagonBoundary(H3IndexValue cell) const noexcept
{
    if (!IsValid(cell))
    {
        return {};
    }

    CellBoundary boundary{};
    if (cellToBoundary(cell, &boundary) != E_SUCCESS)
    {
        return {};
    }

    std::vector<GeoCoordinates> vertices;
    vertices.reserve(boundary.numVerts);

    for (int i = 0; i < boundary.numVerts; ++i)
    {
        vertices.push_back(GeoCoordinates{.latitude = radsToDegs(boundary.verts[i].lat),
                                          .longitude = radsToDegs(boundary.verts[i].lng)});
    }

    return vertices;
}

std::string H3Service::ToString(H3IndexValue cell)
{
    char hex_str[17] = {0};
    if (h3ToString(cell, hex_str, sizeof(hex_str)) != E_SUCCESS)
    {
        return "";
    }
    return std::string(hex_str);
}

std::optional<H3IndexValue> H3Service::FromString(const std::string& h3_str) noexcept
{
    H3Index cell = H3_NULL;
    if (stringToH3(h3_str.c_str(), &cell) != E_SUCCESS || cell == H3_NULL)
    {
        return std::nullopt;
    }
    return cell;
}

} // namespace run::domain::h3