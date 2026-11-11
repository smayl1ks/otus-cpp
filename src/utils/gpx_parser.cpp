#include "utils/gpx_parser.hpp"

#include <charconv>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <pugixml.hpp>
#include <sstream>

namespace run::utils
{

namespace
{

using gameplay::anticheat::TelemetryPoint;

std::optional<float> ParseFloat(const char* text)
{
    if (text == nullptr)
    {
        return std::nullopt;
    }
    float value = 0.0f;
    const auto [ptr, ec] = std::from_chars(text, text + std::strlen(text), value);
    return ec == std::errc{} && ptr == text + std::strlen(text) ? std::optional(value) : std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> ParseIso8601Utc(const char* text)
{
    if (text == nullptr)
    {
        return std::nullopt;
    }
    std::tm tm{};
    std::istringstream stream(text);
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail())
    {
        return std::nullopt;
    }
    tm.tm_isdst = 0;

    // Fractional seconds and trailing zone designator ('Z' or '+hh:mm') are ignored;
    // GPX exporters emit UTC
    const std::time_t epoch = timegm(&tm);
    if (epoch == static_cast<std::time_t>(-1))
    {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(epoch);
}

void ParseTrackPoint(const pugi::xml_node& point, std::vector<TelemetryPoint>& out)
{
    TelemetryPoint telemetry;
    const auto lat = ParseFloat(point.attribute("lat").as_string(nullptr));
    const auto lon = ParseFloat(point.attribute("lon").as_string(nullptr));
    if (!lat.has_value() || !lon.has_value())
    {
        return;
    }
    const auto time = ParseIso8601Utc(point.child("time").text().as_string(nullptr));
    if (!time.has_value())
    {
        return;
    }

    telemetry.coord = {*lat, *lon};
    telemetry.recorded_at = *time;
    telemetry.speed_ms = ParseFloat(point.child("speed").text().as_string(nullptr));
    telemetry.altitude_meters = ParseFloat(point.child("ele").text().as_string(nullptr));
    out.push_back(std::move(telemetry));
}

} // namespace

GpxParseResult ParseGpx(std::string_view gpx_xml)
{
    GpxParseResult result;

    pugi::xml_document document;
    const auto parse_status = document.load_buffer(gpx_xml.data(), gpx_xml.size());
    if (parse_status.status != pugi::status_ok)
    {
        result.error = "Invalid GPX XML: " + std::string(parse_status.description());
        return result;
    }

    const auto root = document.child("gpx");
    if (!root)
    {
        result.error = "Missing <gpx> root element";
        return result;
    }

    for (const auto& track : root.children("trk"))
    {
        for (const auto& segment : track.children("trkseg"))
        {
            for (const auto& point : segment.children("trkpt"))
            {
                ParseTrackPoint(point, result.points);
            }
        }
    }

    for (const auto& route : root.children("rte"))
    {
        for (const auto& point : route.children("rtept"))
        {
            ParseTrackPoint(point, result.points);
        }
    }

    if (result.points.empty())
    {
        result.error = "GPX contains no track points with lat/lon and time";
    }
    return result;
}

} // namespace run::utils
