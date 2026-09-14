#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "../project/MidiTimelineEvents.hpp"
#include "TempoMap.hpp"
#include "TimelineTrack.hpp"

namespace uapmd {

    // The project's tempo and time-signature curve, read off the master track.
    //
    // The master track is the only place this data lives: a clip routed to a regular track has
    // its curve collapsed to a flat reference tempo on import (MidiClipReader::stripToFlatTempo),
    // so nothing else on the timeline can claim tempo authority. Everything derived from that
    // curve is produced here, from one traversal, so the points and the TempoMap can never
    // disagree about what the project's tempo is.
    struct MasterTimelineMeta {
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

        // Timeline-absolute: a clip's own event positions are offset by where the clip sits.
        std::vector<TempoPoint> tempoPoints;
        std::vector<TimeSignaturePoint> timeSignaturePoints;
        double maxTimeSeconds{0.0};
        // The same curve, ready to convert between seconds and quarter-note beats.
        TempoMap tempoMap;

        bool empty() const {
            return tempoPoints.empty() && timeSignaturePoints.empty();
        }
    };

    // Reads the master track's clips in clipId order and builds the whole curve. A null or
    // empty master track yields an empty result, whose TempoMap converts at its default BPM.
    MasterTimelineMeta buildMasterTimelineMeta(
        const std::shared_ptr<TimelineTrack>& masterTrack,
        double sampleRate,
        double defaultBpm = 120.0
    );

}
