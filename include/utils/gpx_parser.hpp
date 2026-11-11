#pragma once

#include "gameplay/anticheat/anticheat.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace run::utils
{

struct GpxParseResult
{
    std::vector<gameplay::anticheat::TelemetryPoint> points;
    std::string error; ///< empty on success
};

/// Parses a GPX document (trk/trkseg/trkpt and rte/rtept) into telemetry points
GpxParseResult ParseGpx(std::string_view gpx_xml);

} // namespace run::utils
