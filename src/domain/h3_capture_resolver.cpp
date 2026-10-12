#include "domain/h3_capture_resolver.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace run::domain::h3
{

H3CaptureResolver::H3CaptureResolver(const H3Service& h3_service, std::size_t min_points_per_h3) noexcept
    : m_h3_service(h3_service), m_min_points_per_h3(min_points_per_h3)
{
}

std::vector<H3IndexValue> H3CaptureResolver::Resolve(const LocationPoints& points) const
{
    if (points.empty())
    {
        return {};
    }

    std::vector<PathPoint> path = BuildPath(points);
    if (path.empty())
    {
        return {};
    }

    std::vector<H3IndexValue> all_claimed;
    all_claimed.reserve(path.size() * 2);

    for (const auto& pt : path)
    {
        all_claimed.push_back(pt.hex);
    }

    AppendLoopsAndEnclosed(path, all_claimed);

    std::sort(all_claimed.begin(), all_claimed.end());
    all_claimed.erase(std::unique(all_claimed.begin(), all_claimed.end()), all_claimed.end());

    return all_claimed;
}

std::vector<H3CaptureResolver::PathPoint> H3CaptureResolver::BuildPath(const LocationPoints& points) const
{
    std::unordered_map<uint64_t, std::size_t> point_counts;
    point_counts.reserve(points.size());

    struct RawPointInfo
    {
        uint64_t hex;
        double lat;
        double lon;
        std::size_t index;
    };

    std::vector<RawPointInfo> raw_sequence;
    raw_sequence.reserve(points.size());

    for (std::size_t i = 0; i < static_cast<std::size_t>(points.size()); ++i)
    {
        const auto& p = points.Get(i);
        const auto h3 = m_h3_service.PointToH3(p.latitude(), p.longitude());
        if (!h3)
        {
            continue;
        }

        point_counts[*h3]++;
        raw_sequence.push_back({*h3, p.latitude(), p.longitude(), i});
    }

    std::unordered_set<uint64_t> qualified_hexes;
    qualified_hexes.reserve(point_counts.size());

    for (const auto& [hex, count] : point_counts)
    {
        if (count >= m_min_points_per_h3)
        {
            qualified_hexes.insert(hex);
        }
    }

    if (qualified_hexes.empty())
    {
        return {};
    }

    std::vector<PathPoint> path;
    path.reserve(raw_sequence.size() * 2);

    for (const auto& raw : raw_sequence)
    {
        if (!qualified_hexes.contains(raw.hex))
        {
            continue;
        }

        if (path.empty())
        {
            path.push_back({raw.hex, raw.lat, raw.lon, raw.index});
            continue;
        }

        const uint64_t last_hex = path.back().hex;
        if (last_hex == raw.hex)
        {
            continue;
        }

        auto sub_path = m_h3_service.GetPathBetweenHexes(last_hex, raw.hex);
        if (sub_path.empty())
        {
            path.push_back({raw.hex, raw.lat, raw.lon, raw.index});
        }
        else
        {
            for (std::size_t k = 1; k < sub_path.size(); ++k)
            {
                path.push_back({sub_path[k], raw.lat, raw.lon, raw.index});
            }
        }
    }

    return path;
}

void H3CaptureResolver::AppendLoopsAndEnclosed(const std::vector<PathPoint>& path,
                                               std::vector<H3IndexValue>& out_claimed) const
{
    if (path.size() < 4)
    {
        return;
    }

    std::size_t i = 0;
    while (i < path.size())
    {
        std::unordered_map<uint64_t, std::size_t> first_seen_index;
        bool found_loop = false;
        std::size_t loop_start = 0;
        std::size_t loop_end = 0;

        for (std::size_t j = i; j < path.size(); ++j)
        {
            uint64_t hex = path[j].hex;
            if (auto it = first_seen_index.find(hex); it != first_seen_index.end())
            {
                loop_start = it->second;
                loop_end = j;
                found_loop = true;
                break;
            }
            first_seen_index[hex] = j;
        }

        if (!found_loop)
        {
            break;
        }

        if (loop_end - loop_start > 5)
        {
            for (std::size_t idx = loop_start; idx <= loop_end; ++idx)
            {
                out_claimed.push_back(path[idx].hex);
            }

            auto enclosed = FillLoop(path, loop_start, loop_end);
            out_claimed.insert(out_claimed.end(), enclosed.begin(), enclosed.end());
        }
        i = loop_end;
    }
}

std::vector<H3IndexValue> H3CaptureResolver::FillLoop(const std::vector<PathPoint>& path, std::size_t start,
                                                      std::size_t end) const
{
    if (end <= start + 2)
    {
        return {};
    }

    std::vector<LatLng> loop_verts;
    loop_verts.reserve((end - start + 1) + 1);

    for (std::size_t i = start; i <= end; ++i)
    {
        loop_verts.push_back({degsToRads(path[i].lat), degsToRads(path[i].lon)});
    }

    if (loop_verts.front().lat != loop_verts.back().lat || loop_verts.front().lng != loop_verts.back().lng)
    {
        loop_verts.push_back(loop_verts.front());
    }

    GeoLoop outer_loop;
    outer_loop.numVerts = static_cast<int>(loop_verts.size());
    outer_loop.verts = loop_verts.data();

    GeoPolygon polygon;
    polygon.geoloop = outer_loop;
    polygon.numHoles = 0;
    polygon.holes = nullptr;

    return m_h3_service.PolygonToHexagons(polygon);
}

} // namespace run::domain::h3