#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ImTimeline.h>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd::gui {

struct ClipPreview {
    struct WaveformPoint {
        float minValue{0.0f};
        float maxValue{0.0f};
        bool hasData{false};
    };

    // Automation event attached to a note (per-note) or to the clip (channel-level).
    struct AutomationEvent {
        enum class Type : uint8_t {
            PitchBend,          // Channel pitch bend (MIDI2: 32-bit)
            PerNotePitchBend,   // Per-note pitch bend (MIDI2 PER_NOTE_PITCH_BEND)
            ChannelPressure,    // Channel aftertouch (CAF, MIDI2: 32-bit)
            PolyPressure,       // Poly aftertouch / per-note pressure (PAF, MIDI2: 32-bit)
            ControlChange,      // CC — paramIndex = CC# (0-127); value: MIDI2 32-bit
            RPN,                // Registered Controller — paramIndex = (MSB<<7)|LSB
            NRPN,               // Assignable Controller — paramIndex = (MSB<<7)|LSB; maps to plugin param via index=MSB*128+LSB
            PerNoteParameter,   // Per-note RCC / ACC (MIDI2)
        };
        double timeSeconds{0.0};    // Absolute time within the clip
        double normalizedValue{0.0}; // Value in [0, 1]
        Type type{Type::ControlChange};
        uint8_t channel{0};
        uint8_t noteNumber{0};  // For per-note event types
        uint8_t umpGroup{0};    // UMP group (0–15); for NRPN selects which plugin instance on the track
        uint16_t paramIndex{0}; // CC number / RPN number / NRPN number
        size_t rawEventIdx{SIZE_MAX}; // first-word index in RawMidiData::umpEvents (SIZE_MAX = new/synthetic)
    };

    struct MidiNote {
        double startSeconds{0.0};
        double durationSeconds{0.0};
        uint8_t note{0};
        float   velocity{0.0f};             // normalized 0.0–1.0 (MIDI1→/127, MIDI2→/65535)
        uint8_t channel{0};
        bool    isMidi2{false};             // true if from a 2-word MIDI2 UMP
        bool    deleted{false};             // marked for removal on next write-back
        size_t  noteOnWordIdx{SIZE_MAX};    // index of NoteOn first word in RawMidiData
        size_t  noteOffWordIdx{SIZE_MAX};   // index of NoteOff first word in RawMidiData
    };

    // Raw MIDI source data retained for piano-roll write-back.
    struct RawMidiData {
        std::vector<uapmd_ump_t> umpEvents;     // word-per-entry (same layout as MidiClipSourceNode)
        std::vector<uint64_t>    tickTimestamps; // tick per word (original SMF ticks)
        uint32_t tickResolution{480};            // PPQ from SMF header
        double   clipTempo{120.0};               // original clip BPM
    };

    bool isMidiClip{false};
    bool isMasterMeta{false};
    bool ready{false};
    bool hasError{false};
    std::string errorMessage;
    std::string signature;
    std::string displayName;
    double clipDurationSeconds{0.0};
    int64_t sourceDurationSamples{0};
    std::vector<WaveformPoint> waveform;
    std::vector<uapmd::ClipMarker> clipMarkers;
    std::vector<std::string> audioWarpReferenceLabels;
    std::vector<uapmd::AudioWarpPoint> audioWarps;
    std::vector<MidiNote> midiNotes;
    uint8_t minNote{48};
    uint8_t maxNote{72};
    struct TempoPoint {
        double timeSeconds{0.0};
        double bpm{120.0};
    };
    struct TimeSignaturePoint {
        double timeSeconds{0.0};
        uint8_t numerator{4};
        uint8_t denominator{4};
    };
    std::vector<TempoPoint> tempoPoints;
    std::vector<TimeSignaturePoint> timeSignaturePoints;
    // Raw MIDI source data for piano-roll write-back (null for audio / master-meta clips).
    std::shared_ptr<RawMidiData> rawMidiData;
};

std::shared_ptr<ClipPreview> createAudioClipPreview(
    const std::string& filepath,
    double fallbackDurationSeconds,
    const uapmd::ClipData* clipData
);

std::shared_ptr<ClipPreview> createMidiClipPreview(
    int32_t trackIndex,
    const uapmd::ClipData& clipData,
    double fallbackDurationSeconds
);

std::shared_ptr<CustomNodeBase> createClipContentNode(
    std::shared_ptr<ClipPreview> preview,
    float uiScale,
    const std::string& clipName
);

std::shared_ptr<ClipPreview> createMasterMetaPreview(
    std::vector<ClipPreview::TempoPoint> tempoPoints,
    std::vector<ClipPreview::TimeSignaturePoint> timeSignaturePoints,
    double durationSeconds
);

} // namespace uapmd::gui
