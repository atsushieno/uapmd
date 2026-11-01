#pragma once

#include <bit>
#include <cstdint>

namespace uapmd {

    // MIDI stores the denominator as an exponent (2 means /4); the timeline
    // stores the actual note value. Limit decoding to what uint8_t can hold.
    inline uint8_t decodeMidiTimeSignatureDenominator(uint8_t exponent) {
        return static_cast<uint8_t>(1u << (exponent > 7 ? 7 : exponent));
    }

    inline uint8_t encodeMidiTimeSignatureDenominator(uint8_t denominator) {
        if (!std::has_single_bit(denominator))
            return 2; // Fall back to quarter notes for an unsupported denominator.
        return static_cast<uint8_t>(std::countr_zero(denominator));
    }

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
