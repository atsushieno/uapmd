#pragma once

#include <string>

#include <uapmd-data/detail/project/LatencyCompensationTypes.hpp>

namespace uapmd {
    typedef int32_t uapmd_track_index_t;

    enum class OutputAlignmentMonitoringPolicy {
        LOW_LATENCY_LIVE_INPUT = 0,
        FULLY_COMPENSATED = 1,
    };

    enum class RealtimeInfiniteTailPolicy {
        LATENCY_FALLBACK = 0,
        IMMEDIATE_STOP = 1,
    };

    class LatencyCompensationManager {
    public:
        virtual ~LatencyCompensationManager() = default;

        virtual bool trackRecordArmed(uapmd_track_index_t trackIndex) const = 0;
        virtual void trackRecordArmed(uapmd_track_index_t trackIndex, bool armed) = 0;
        virtual bool trackMonitoringEnabled(uapmd_track_index_t trackIndex) const = 0;
        virtual void trackMonitoringEnabled(uapmd_track_index_t trackIndex, bool enabled) = 0;
        virtual PlaybackCompensationMode playbackCompensationMode() const = 0;
        virtual void playbackCompensationMode(PlaybackCompensationMode mode) = 0;
        virtual InputMonitoringPolicy inputMonitoringPolicy() const = 0;
        virtual void inputMonitoringPolicy(InputMonitoringPolicy policy) = 0;
        virtual LatencyCompensationProjectSettings projectSettings() const = 0;
        virtual bool applyProjectSettings(
            const LatencyCompensationProjectSettings& settings,
            std::string& error) = 0;
        virtual OutputAlignmentMonitoringPolicy outputAlignmentMonitoringPolicy() const = 0;
        virtual void outputAlignmentMonitoringPolicy(OutputAlignmentMonitoringPolicy policy) = 0;
        virtual RealtimeInfiniteTailPolicy realtimeInfiniteTailPolicy() const = 0;
        virtual void realtimeInfiniteTailPolicy(RealtimeInfiniteTailPolicy policy) = 0;
    };
}
