#pragma once

#include "domain/h3_service.hpp"
#include "location.pb.h"

#include <algorithm>

namespace run::domain::validator
{

struct RunValidationConfig
{
    double min_distance_meters{100.0};
    int64_t min_duration_seconds{60};

    double max_distance_meters{150000.0}; // Max 150 km per run
    int64_t max_duration_seconds{43200};  // Max 12 hours per session
};

struct RunMetrics
{
    double total_distance_meters{0.0};
    int64_t duration_seconds{0};
    bool is_valid{false};
};

class RunValidator
{
public:
    explicit RunValidator(RunValidationConfig config = {}) : m_config(config)
    {
    }

    RunMetrics
    Validate(const google::protobuf::RepeatedPtrField<run::proto::location::LocationPoint>& points) const noexcept
    {
        RunMetrics metrics;
        if (points.size() < 2)
        {
            return metrics;
        }

        for (int i = 1; i < points.size(); ++i)
        {
            const auto& prev = points.Get(i - 1);
            const auto& curr = points.Get(i);
            metrics.total_distance_meters +=
                h3::H3Service::GetDistanceMeters(prev.latitude(), prev.longitude(), curr.latitude(), curr.longitude());
        }

        const int64_t start_time = points.Get(0).timestamp();
        const int64_t end_time = points.Get(points.size() - 1).timestamp();
        metrics.duration_seconds = std::max<int64_t>(0, (end_time - start_time) / 1000);

        const bool is_distance_valid = (metrics.total_distance_meters >= m_config.min_distance_meters) &&
                                       (metrics.total_distance_meters <= m_config.max_distance_meters);

        const bool is_duration_valid = (metrics.duration_seconds >= m_config.min_duration_seconds) &&
                                       (metrics.duration_seconds <= m_config.max_duration_seconds);

        metrics.is_valid = is_distance_valid && is_duration_valid;

        return metrics;
    }

private:
    RunValidationConfig m_config;
};

} // namespace run::domain::validator
