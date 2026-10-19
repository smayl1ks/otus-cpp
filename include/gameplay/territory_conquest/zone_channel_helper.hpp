#pragma once

#include <charconv>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace run::gameplay::territory_conquest
{

class ZoneChannelHelper
{
public:
    static std::string FormatChannel(uint64_t h3_zone, std::string_view city_prefix = "kazan")
    {
        return fmt::format("{}:zone:{:x}", city_prefix, h3_zone);
    }

    static uint64_t ExtractH3Zone(std::string_view channel_name, std::string_view prefix = "kazan:zone:")
    {
        if (!channel_name.starts_with(prefix))
        {
            return 0;
        }

        std::string_view zone_view = channel_name.substr(prefix.size());
        uint64_t h3_zone = 0;

        auto [ptr, ec] = std::from_chars(zone_view.data(), zone_view.data() + zone_view.size(), h3_zone, 16);

        if (ec != std::errc{})
        {
            return 0;
        }

        return h3_zone;
    }
};

} // namespace run::gameplay::territory_conquest
