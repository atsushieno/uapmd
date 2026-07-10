#pragma once

#include <cstdint>
#include <vector>

namespace uapmd {

    enum class InfiniteTailDrainFallback {
        ZERO = 0,
        LATENCY_FALLBACK = 1,
    };

    struct StopDrainPathInfo {
        uint32_t latency_in_samples{0};
        double tail_length_in_seconds{0.0};
    };

    int64_t computeStopDrainFrames(
        int32_t sampleRate,
        uint32_t masterRenderLeadInSamples,
        double masterTailLengthInSeconds,
        uint32_t latencyFallbackSamples,
        InfiniteTailDrainFallback infiniteTailFallback,
        const std::vector<StopDrainPathInfo>& paths);

} // namespace uapmd
