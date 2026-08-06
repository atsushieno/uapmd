#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace uapmd {
    enum class PlaybackCompensationMode {
        COMPENSATED = 0,
        LOW_LATENCY = 1,
    };

    enum class InputMonitoringPolicy {
        // Prefer monitored live input over fully compensated playback on that path
        // when the track is both record-armed and monitor-enabled.
        TAPE_STYLE = 0,
        // Use low-latency monitoring only for tracks that have live input and are
        // explicitly monitor-enabled. Other playback remains compensated.
        AUTO = 1,
        // Disable live-input monitoring through the latency-compensation layer.
        OFF = 2,
    };

    struct LatencyCompensationProjectSettings {
        std::string implementation_id{"default"};
        PlaybackCompensationMode playback_compensation_mode{PlaybackCompensationMode::COMPENSATED};
        InputMonitoringPolicy input_monitoring_policy{InputMonitoringPolicy::AUTO};
        std::vector<int32_t> monitored_track_indexes{};
        std::vector<int32_t> record_armed_track_indexes{};
        std::map<std::string, std::string> implementation_properties{};
    };
}
