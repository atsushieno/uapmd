#pragma once

#include "SourceNode.hpp"
#include <cstdint>

namespace remidy { class EventSequence; }

namespace uapmd {

    // Abstract base class for MIDI event generating sources
    // Parallel to AudioSourceNode hierarchy for MIDI content
    class MidiSourceNode : public SourceNode {
    public:
        virtual ~MidiSourceNode() = default;

        // Playback control
        virtual void seek(int64_t samplePosition) = 0;
        virtual int64_t currentPosition() const = 0;
        virtual int64_t totalLength() const = 0;
        virtual bool isPlaying() const = 0;
        virtual void setPlaying(bool playing) = 0;

        // MIDI event generation
        // Appends UMP events to EventSequence for the current frame window
        // frameCount: number of samples in this processing chunk
        // sampleRate: current sample rate
        // tempo: current timeline tempo in BPM
        virtual void processEvents(
            remidy::EventSequence& eventOut,
            int32_t frameCount,
            int32_t sampleRate,
            double tempo
        ) = 0;
    };

} // namespace uapmd
