#pragma once

#include <cstdint>
#include <vector>

namespace uapmd {

    // Node type enumeration for timeline source nodes
    enum class SourceNodeType {
        AudioFileSource, // Audio file clip playback
        DeviceInput,     // Device input capture
        MidiClipSource,  // MIDI file clip playback
        Generator        // Future: synth/oscillator
    };

    // Base interface for all timeline source nodes
    class SourceNode {
    public:
        virtual ~SourceNode() = default;

        // Node identification
        virtual int32_t instanceId() const = 0;
        virtual SourceNodeType nodeType() const = 0;

        // Bypass control
        virtual bool disabled() const = 0;
        virtual void disabled(bool value) = 0;

        // State management
        virtual std::vector<uint8_t> saveState() = 0;
        virtual void loadState(const std::vector<uint8_t>& state) = 0;
    };

} // namespace uapmd
