#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <uapmd/uapmd.hpp>

#include "../midi/MidiTimelineEvents.hpp"

namespace uapmd::import {

struct MidiTrackImport {
    std::string clipName;
    std::vector<uapmd_ump_t> umpEvents;
    std::vector<uint64_t> umpTickTimestamps;
    uint32_t tickResolution{0};
    double detectedTempo{120.0};
    std::vector<MidiTempoChange> tempoChanges;
    std::vector<MidiTimeSignatureChange> timeSignatureChanges;
    bool needsFileSave{false};
};

struct MidiImportResult {
    bool success{false};
    std::string error;
    std::vector<std::string> warnings;
    std::vector<MidiTrackImport> tracks;
    std::vector<MidiTrackImport> masterTrackClips;
};

struct AudioStemImport {
    std::string stemName;
    std::filesystem::path filepath;
    std::string clipDisplayName;
};

struct AudioImportResult {
    bool success{false};
    bool canceled{false};
    std::string error;
    std::vector<std::string> warnings;
    std::vector<AudioStemImport> stems;
};

class TrackImporter {
public:
    static MidiImportResult importMidiFile(const std::string& filepath);

    struct AudioImportOptions {
        std::string modelPath;
        std::filesystem::path outputDirectory;
        std::function<void(float /*progress*/, const std::string& /*message*/)> progressCallback;
        std::function<bool()> shouldCancel;
    };

    static AudioImportResult importAudioFile(const std::string& filepath,
                                             const AudioImportOptions& options);
};

} // namespace uapmd::import
