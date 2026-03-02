#pragma once

#include <cstdint>

namespace uapmd {

    // Represents a tempo change extracted from an SMF meta event.
    struct MidiTempoChange {
        uint64_t tickPosition{0};
        double bpm{120.0};
    };

    // Represents a time signature change extracted from an SMF meta event.
    struct MidiTimeSignatureChange {
        uint64_t tickPosition{0};
        uint8_t numerator{4};
        uint8_t denominator{4};
        uint8_t clocksPerClick{24};
        uint8_t thirtySecondsPerQuarter{8};
    };

} // namespace uapmd
