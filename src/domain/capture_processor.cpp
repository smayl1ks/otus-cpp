#include "domain/capture_processor.hpp"
#include <algorithm>

namespace run::domain::h3
{

CaptureProcessor::CaptureProcessor(const H3Service& h3_service, std::size_t min_points_per_hex) noexcept
    : m_h3_service(h3_service), m_min_points_per_hex(min_points_per_hex)
{
}

CaptureResult CaptureProcessor::ProcessRun(
    const google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points) const
{
    CaptureResult result;
    if (points.empty())
    {
        return result;
    }

    result.path_hexes = BuildInterpolatedPath(points);
    if (result.path_hexes.empty())
    {
        return result;
    }

    auto loop_data = DetectLoopsAndFill(result.path_hexes);
    result.boundary_hexes = std::move(loop_data.boundary);
    result.enclosed_hexes = std::move(loop_data.enclosed);

    std::unordered_set<uint64_t> unique_total;
    unique_total.insert(result.path_hexes.begin(), result.path_hexes.end());
    unique_total.insert(result.boundary_hexes.begin(), result.boundary_hexes.end());
    unique_total.insert(result.enclosed_hexes.begin(), result.enclosed_hexes.end());

    result.total_claimed.assign(unique_total.begin(), unique_total.end());

    return result;
}

std::vector<uint64_t> CaptureProcessor::BuildInterpolatedPath(
    const google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points) const
{
    std::unordered_map<uint64_t, std::size_t> point_counts;
    point_counts.reserve(points.size());

    std::vector<uint64_t> raw_hex_sequence;
    raw_hex_sequence.reserve(points.size());

    for (const auto& point : points)
    {
        const auto current_h3 = m_h3_service.PointToH3(point.latitude(), point.longitude());
        if (!current_h3)
        {
            continue;
        }

        point_counts[*current_h3]++;
        raw_hex_sequence.push_back(*current_h3);
    }

    std::unordered_set<uint64_t> qualified_hexes;
    qualified_hexes.reserve(point_counts.size());

    for (const auto& [hex, count] : point_counts)
    {
        if (count >= m_min_points_per_hex)
        {
            qualified_hexes.insert(hex);
        }
    }

    if (qualified_hexes.empty())
    {
        return {};
    }

    std::vector<uint64_t> visited_hexes;
    visited_hexes.reserve(raw_hex_sequence.size() * 2);

    for (const auto h3_cell : raw_hex_sequence)
    {
        if (!qualified_hexes.contains(h3_cell))
        {
            continue;
        }

        if (visited_hexes.empty())
        {
            visited_hexes.push_back(h3_cell);
            continue;
        }

        const uint64_t last_h3 = visited_hexes.back();
        if (last_h3 == h3_cell)
        {
            continue;
        }

        auto path = m_h3_service.GetPathBetweenHexes(last_h3, h3_cell);
        if (path.empty())
        {
            visited_hexes.push_back(h3_cell);
        }
        else
        {
            visited_hexes.insert(visited_hexes.end(), std::make_move_iterator(path.begin() + 1),
                                 std::make_move_iterator(path.end()));
        }
    }

    return visited_hexes;
}

CaptureProcessor::LoopData CaptureProcessor::DetectLoopsAndFill(const std::vector<uint64_t>& path_hexes) const
{
    LoopData data;
    if (path_hexes.size() < 4)
    {
        return data;
    }

    std::unordered_map<uint64_t, size_t> first_seen_index;
    size_t loop_start_idx = 0;
    size_t loop_end_idx = 0;
    bool found_loop = false;

    for (size_t i = 0; i < path_hexes.size(); ++i)
    {
        uint64_t hex = path_hexes[i];
        if (first_seen_index.contains(hex))
        {
            loop_start_idx = first_seen_index[hex];
            loop_end_idx = i;
            found_loop = true;
            break;
        }
        else
        {
            first_seen_index[hex] = i;
        }
    }

    if (!found_loop)
    {
        return data;
    }

    std::vector<uint64_t> boundary;
    for (size_t i = loop_start_idx; i <= loop_end_idx; ++i)
    {
        boundary.push_back(path_hexes[i]);
    }
    data.boundary = boundary;

    // Внутренние гексагоны (заливка контура)
    std::unordered_set<uint64_t> enclosed;
    // ... логика заливки полигона внутри boundary ...

    data.enclosed.assign(enclosed.begin(), enclosed.end());
    return data;
}

} // namespace run::domain::h3
