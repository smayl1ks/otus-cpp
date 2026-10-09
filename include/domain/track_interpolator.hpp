#pragma once

#include "domain/h3_service.hpp"
#include "location.pb.h"
#include <vector>

namespace run::domain::h3
{

class TrackInterpolator
{
public:
    explicit TrackInterpolator(const H3Service& h3_service) noexcept : m_h3_service(h3_service)
    {
    }

    std::vector<H3IndexValue>
    InterpolateTrack(const google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points) const
    {
        if (points.empty())
        {
            return {};
        }

        std::vector<H3IndexValue> visited_hexes;
        visited_hexes.reserve(static_cast<std::size_t>(points.size()) * 2);

        for (const auto& point : points)
        {
            const auto current_h3 = m_h3_service.PointToH3(point.latitude(), point.longitude());
            if (!current_h3)
            {
                continue;
            }

            if (visited_hexes.empty())
            {
                visited_hexes.push_back(*current_h3);
                continue;
            }

            const H3IndexValue last_h3 = visited_hexes.back();

            if (last_h3 == *current_h3)
            {
                continue;
            }

            auto path = m_h3_service.GetPathBetweenHexes(last_h3, *current_h3);

            if (path.empty())
            {
                visited_hexes.push_back(*current_h3);
            }
            else
            {
                visited_hexes.insert(visited_hexes.end(), std::make_move_iterator(path.begin() + 1),
                                     std::make_move_iterator(path.end()));
            }
        }

        return visited_hexes;
    }

private:
    const H3Service& m_h3_service;
};

} // namespace run::domain::h3