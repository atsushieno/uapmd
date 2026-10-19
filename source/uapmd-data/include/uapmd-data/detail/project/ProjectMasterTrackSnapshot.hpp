#pragma once

#include <cstdint>
#include <vector>
#include "MidiTimelineEvents.hpp"

namespace uapmd {
    struct ProjectMasterTrackSnapshot {
        struct TempoPoint {
            double timeSeconds{0.0};
            uint64_t tickPosition{0};
            double bpm{0.0};
        };
        struct TimeSignaturePoint {
            double timeSeconds{0.0};
            uint64_t tickPosition{0};
            MidiTimeSignatureChange signature{};
        };
        std::vector<TempoPoint> tempoPoints;
        std::vector<TimeSignaturePoint> timeSignaturePoints;
        double maxTimeSeconds{0.0};
        bool empty() const {
            return tempoPoints.empty() && timeSignaturePoints.empty();
        }
    };

}
