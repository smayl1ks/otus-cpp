#include "gameplay/anticheat/anticheat.hpp"
#include "utils/gpx_parser.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>

namespace
{

using namespace run::gameplay::anticheat;

// Real-world track fixture (tests/real_track.gpx, ~3300 points from a recorded run)
constexpr const char* kRealTrackGpxPath = RUN_SOURCE_DIR "/tests/real_track.gpx";

std::string ReadFileOrEmpty(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

run::utils::GpxParseResult LoadRealTrack()
{
    return run::utils::ParseGpx(ReadFileOrEmpty(kRealTrackGpxPath));
}

void BM_AnticheatValidateRealTrack(benchmark::State& state)
{
    const auto parsed = LoadRealTrack();
    if (parsed.points.empty())
    {
        state.SkipWithError("real track fixture not found or unparseable");
        return;
    }

    const auto& telemetry = parsed.points;
    // Live config rejects batches of >100 points; relax it so the recorded track is fully validated
    GpsAntiCheatConfig config;
    config.max_points_per_batch = telemetry.size();
    GpsAntiCheat anticheat(config);
    const int64_t now_ms = telemetry.back().RecordedAtMs() + 60'000;
    for (auto _ : state)
    {
        auto status = anticheat.ValidateBatch(std::span{telemetry}, now_ms);
        benchmark::DoNotOptimize(status);
    }
    state.counters["points/s"] = benchmark::Counter(static_cast<double>(telemetry.size()),
                                                    benchmark::Counter::kIsIterationInvariantRate);
}
BENCHMARK(BM_AnticheatValidateRealTrack);

} // namespace
