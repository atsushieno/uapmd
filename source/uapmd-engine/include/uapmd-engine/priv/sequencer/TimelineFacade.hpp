#pragma once
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <uapmd/uapmd.hpp>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

class SequencerEngine;

// Facade for timeline clip management and project loading.
// Owned by SequencerEngineImpl; accessed via SequencerEngine::timeline().
class TimelineFacade {
protected:
    TimelineFacade() = default;

public:
    virtual ~TimelineFacade() = default;

    // Global timeline state (tempo, time signature, playhead, loop)
    virtual TimelineState& state() = 0;

    // One TimelineTrack* per SequencerTrack, at the same index
    virtual std::vector<TimelineTrack*> tracks() = 0;

    // Clip management
    struct ClipAddResult {
        int32_t clipId{-1};
        int32_t sourceNodeId{-1};
        bool success{false};
        std::string error;
    };

    virtual ClipAddResult addClipToTrack(
        int32_t trackIndex,
        const TimelinePosition& position,
        std::unique_ptr<AudioFileReader> reader,
        const std::string& filepath = "") = 0;

    virtual ClipAddResult addMidiClipToTrack(
        int32_t trackIndex,
        const TimelinePosition& position,
        const std::string& filepath) = 0;

    virtual ClipAddResult addMidiClipToTrack(
        int32_t trackIndex,
        const TimelinePosition& position,
        std::vector<uapmd_ump_t> umpEvents,
        std::vector<uint64_t> umpTickTimestamps,
        uint32_t tickResolution,
        double clipTempo,
        std::vector<MidiTempoChange> tempoChanges,
        std::vector<MidiTimeSignatureChange> timeSignatureChanges,
        const std::string& clipName = "") = 0;

    virtual bool removeClipFromTrack(int32_t trackIndex, int32_t clipId) = 0;

    // Project loading (synchronous; blocks until all plugin instantiations complete)
    struct ProjectResult {
        bool success{false};
        std::string error;
    };

    virtual ProjectResult loadProject(const std::filesystem::path& file) = 0;

    // Tempo map and time-signature metadata extracted from master track MIDI clips
    struct MasterTrackSnapshot {
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

    virtual MasterTrackSnapshot buildMasterTrackSnapshot() = 0;

    struct ContentBounds {
        bool hasContent{false};
        int64_t firstSample{0};
        int64_t lastSample{0};
        double firstSeconds{0.0};
        double lastSeconds{0.0};

        double durationSeconds() const {
            return hasContent ? std::max(0.0, lastSeconds - firstSeconds) : 0.0;
        }
    };

    virtual ContentBounds calculateContentBounds() const = 0;

    // Audio preprocess callback — feeds clip source nodes into track input buffers.
    // Called by SequencerEngineImpl via the registered AudioPreprocessCallback.
    virtual void processTracksAudio(AudioProcessContext& process) = 0;

    // Lifecycle hooks called by SequencerEngineImpl when tracks are added/removed
    virtual void onTrackAdded(uint32_t outputChannels,
                              double sampleRate,
                              uint32_t bufferSizeInFrames) = 0;
    virtual void onTrackRemoved(size_t trackIndex) = 0;

    static std::unique_ptr<TimelineFacade> create(SequencerEngine& engine);
};

} // namespace uapmd
