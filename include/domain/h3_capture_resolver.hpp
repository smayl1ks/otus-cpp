#pragma once

#include "domain/h3_service.hpp"
#include "location.pb.h"

#include <vector>

namespace run::domain::h3
{

class H3CaptureResolver
{
public:
    struct PathPoint
    {
        H3IndexValue hex;
        double lat;
        double lon;
        std::size_t original_index;
    };

    using LocationPoints = google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>;

    explicit H3CaptureResolver(const H3Service& h3_service, std::size_t min_points_per_h3 = 2) noexcept;

    std::vector<H3IndexValue> Resolve(const LocationPoints& points) const;

private:
    std::vector<PathPoint> BuildPath(const LocationPoints& points) const;

    void AppendLoopsAndEnclosed(const std::vector<PathPoint>& path, std::vector<H3IndexValue>& out_claimed) const;

    std::vector<H3IndexValue> FillLoop(const std::vector<PathPoint>& path, std::size_t start, std::size_t end) const;

private:
    const H3Service& m_h3_service;
    std::size_t m_min_points_per_h3{2};
};

} // namespace run::domain::h3