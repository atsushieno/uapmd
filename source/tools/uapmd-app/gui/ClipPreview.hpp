#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ImTimeline.h>
#include <uapmd-data/uapmd-data.hpp>
#include <uapmd-app-model/uapmd-app-model.hpp>

namespace uapmd_app_gui {

struct ClipPreview {
    struct WaveformPoint {
        float minValue{0.0f};
        float maxValue{0.0f};
        bool hasData{false};
    };

    using AutomationEvent = uapmd_app::PianoRollAutomationEvent;
    using MidiNote = uapmd_app::PianoRollMidiNote;
    using RawMidiData = uapmd_app::PianoRollRawMidiData;

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

    // Where a clip-relative instant sits across the clip's drawn width, as a fraction.
    //
    // The box a preview draws into is only linear in seconds when the timeline's ruler is: on a
    // bars-and-beats ruler the same instant lands somewhere else entirely, and under a varying
    // tempo the two disagree by a different amount at every point. So the owner supplies the
    // conversion instead of the preview assuming its width is seconds.
    std::function<double(double)> timeToFraction;

    double fractionAt(double clipRelativeSeconds) const {
        if (timeToFraction)
            return timeToFraction(clipRelativeSeconds);
        return clipDurationSeconds > 0.0 ? clipRelativeSeconds / clipDurationSeconds : 0.0;
    }
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

} // namespace uapmd_app_gui
