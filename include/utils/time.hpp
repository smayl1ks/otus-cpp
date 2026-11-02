#pragma once

// Wall-clock helpers shared by services and the network layer. A single
// definition instead of a per-service static: same timestamp semantics
// everywhere, injectable later if tests ever need a fake clock.

#include <chrono>
#include <cstdint>

namespace run::utils
{

/// Milliseconds since the Unix epoch — the project-wide timestamp unit
/// (proto fields, run records, Redis keys).
inline int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace run::utils
