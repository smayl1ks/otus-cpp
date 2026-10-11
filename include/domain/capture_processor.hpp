#pragma once

#include "domain/h3_service.hpp"
#include "location.pb.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace run::domain::h3
{

struct CaptureResult
{
    std::vector<uint64_t> path_hexes;     // Линия пробежки (интерполированный трек)
    std::vector<uint64_t> boundary_hexes; // Границы замкнутых петель
    std::vector<uint64_t> enclosed_hexes; // Захваченная внутренняя территория внутри петель
    std::vector<uint64_t> total_claimed;  // Итоговый уникальный набор всех захваченных гексагонов
};

class CaptureProcessor
{
public:
    explicit CaptureProcessor(const H3Service& h3_service, std::size_t min_points_per_hex = 2) noexcept;

    CaptureResult
    ProcessRun(const google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points) const;

private:
    std::vector<uint64_t>
    BuildInterpolatedPath(const google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points) const;

    struct LoopData
    {
        std::vector<uint64_t> boundary;
        std::vector<uint64_t> enclosed;
    };

    LoopData DetectLoopsAndFill(const std::vector<uint64_t>& path_hexes) const;

private:
    const H3Service& m_h3_service;
    std::size_t m_min_points_per_hex{2};
};

} // namespace run::domain::h3