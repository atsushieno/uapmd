#pragma once

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

    struct MidiNote {
        double startSeconds{0.0};
        double durationSeconds{0.0};
        uint8_t note{0};
        uint8_t velocity{0};
        uint8_t channel{0};
    };

    bool isMidiClip{false};
    bool isMasterMeta{false};
    bool ready{false};
    bool hasError{false};
    std::string errorMessage;
    std::string signature;
    std::string displayName;
    double clipDurationSeconds{0.0};
    std::vector<WaveformPoint> waveform;
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
