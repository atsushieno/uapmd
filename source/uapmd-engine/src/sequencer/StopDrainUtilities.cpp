#include "StopDrainUtilities.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace uapmd {
    namespace {
        double tailLengthSecondsToSamples(double seconds, int32_t sampleRate) {
            if (!(seconds > 0.0))
                return 0.0;
            if (!std::isfinite(seconds))
                return std::numeric_limits<double>::infinity();
            return std::ceil(seconds * static_cast<double>(sampleRate));
        }
    }

    int64_t computeStopDrainFrames(
        int32_t sampleRate,
        uint32_t masterRenderLeadInSamples,
        double masterTailLengthInSeconds,
        uint32_t latencyFallbackSamples,
        InfiniteTailDrainFallback infiniteTailFallback,
        const std::vector<StopDrainPathInfo>& paths) {
        double totalSamples =
            static_cast<double>(masterRenderLeadInSamples) +
            tailLengthSecondsToSamples(masterTailLengthInSeconds, sampleRate);

        for (const auto& path : paths) {
            const double pathSamples =
                static_cast<double>(path.latency_in_samples) +
                tailLengthSecondsToSamples(path.tail_length_in_seconds, sampleRate);
            if (!std::isfinite(pathSamples))
                return infiniteTailFallback == InfiniteTailDrainFallback::LATENCY_FALLBACK
                    ? static_cast<int64_t>(latencyFallbackSamples)
                    : 0;
            totalSamples = std::max(totalSamples, pathSamples);
        }

        if (!std::isfinite(totalSamples))
            return infiniteTailFallback == InfiniteTailDrainFallback::LATENCY_FALLBACK
                ? static_cast<int64_t>(latencyFallbackSamples)
                : 0;
        if (totalSamples <= 0.0)
            return 0;
        if (totalSamples >= static_cast<double>(std::numeric_limits<int64_t>::max()))
            return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(totalSamples);
    }

} // namespace uapmd
