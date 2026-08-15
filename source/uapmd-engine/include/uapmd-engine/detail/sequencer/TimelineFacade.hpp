#pragma once
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <uapmd-midi-service/uapmd-midi-service.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include "SequenceProcessContext.hpp"
#include "ProjectUndo.hpp"
#include "LatencyCompensationTypes.hpp"

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
    virtual TimelineTrack* masterTimelineTrack() = 0;
    virtual int32_t trackIndexForReferenceId(std::string_view trackId) const = 0;

    // Clip management
    struct ClipAddResult {
        int32_t clipId{-1};
        int32_t sourceNodeId{-1};
        bool success{false};
        std::string error;
    };

    virtual ClipAddResult addAudioClipToTrack(
        int32_t trackIndex,
        const TimelinePosition& position,
        std::unique_ptr<AudioFileReader> reader,
        const std::string& filepath = "",
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual ClipAddResult addMidiClipToTrack(
        int32_t trackIndex,
        const TimelinePosition& position,
        const std::string& filepath,
        bool nrpnToParameterMapping = false,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual ClipAddResult addMidiClipToTrack(
        int32_t trackIndex,
        const TimelinePosition& position,
        std::vector<uapmd_ump_t> umpEvents,
        std::vector<uint64_t> umpTickTimestamps,
        uint32_t tickResolution,
        double clipTempo,
        std::vector<MidiTempoChange> tempoChanges,
        std::vector<MidiTimeSignatureChange> timeSignatureChanges,
        const std::string& clipName = "",
        bool nrpnToParameterMapping = false,
        bool needsFileSave = false,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual ClipAddResult addMasterMidiClip(
        const TimelinePosition& position,
        std::vector<uapmd_ump_t> umpEvents,
        std::vector<uint64_t> umpTickTimestamps,
        uint32_t tickResolution,
        double clipTempo,
        std::vector<MidiTempoChange> tempoChanges,
        std::vector<MidiTimeSignatureChange> timeSignatureChanges,
        const std::string& clipName = "",
        bool needsFileSave = false,
        const std::string& filepath = "",
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual bool removeClipFromTrack(
        int32_t trackIndex,
        int32_t clipId,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    // Removes every clip on the track. Returns true when at least one was removed.
    virtual bool clearClipsFromTrack(
        int32_t trackIndex,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool notifyClipChanged(int32_t trackIndex, int32_t clipId, std::string type = "clip-changed") = 0;

    // Clip mutations.
    //
    // These are the only supported way to change a clip: each applies the
    // change and emits the matching document event as one step, so that an
    // observer can never miss a mutation. Callers must not reach into
    // TimelineTrack::clipManager() to mutate a clip directly -- doing so
    // leaves observers, and eventually the undo stack, out of sync.
    // Read-only access through clipManager() remains fine.
    //
    // Each returns false when the track or clip does not exist, or when the
    // underlying change was rejected.
    virtual bool setClipEnabled(int32_t trackIndex, int32_t clipId, bool enabled,
                                ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipAnchor(int32_t trackIndex, int32_t clipId, const TimeReference& anchor,
                               ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipGain(int32_t trackIndex, int32_t clipId, double gain,
                             ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipMuted(int32_t trackIndex, int32_t clipId, bool muted,
                              ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool resizeClip(int32_t trackIndex, int32_t clipId, int64_t newDurationSamples,
                            ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipName(int32_t trackIndex, int32_t clipId, const std::string& name,
                             ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipFilepath(int32_t trackIndex, int32_t clipId, const std::string& filepath,
                                 ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipNeedsFileSave(int32_t trackIndex, int32_t clipId, bool needsSave,
                                      ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipMarkers(int32_t trackIndex, int32_t clipId, std::vector<ClipMarker> markers,
                                ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipAudioWarps(int32_t trackIndex, int32_t clipId, std::vector<AudioWarpPoint> audioWarps,
                                   ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual bool clipEnabled(int32_t trackIndex, int32_t clipId) const = 0;

    // Rebuilds an audio clip's source from its file with the given markers and
    // warp points, resolving warp references against the whole project.
    //
    // A non-empty `filepath` switches the clip to that file and adopts its
    // length; otherwise the clip keeps its length, because a warp rebuild
    // changes what plays rather than how long the clip is.
    //
    // `masterTrackMarkers` is passed in because markers and warps may reference
    // the master track's markers, which the engine does not own.
    virtual bool replaceAudioClipContent(
        int32_t trackIndex,
        int32_t clipId,
        const std::string& filepath,
        std::vector<ClipMarker> markers,
        std::vector<AudioWarpPoint> audioWarps,
        const std::vector<ClipMarker>& masterTrackMarkers,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Replaces a MIDI clip's authored content, resizing the clip to match the
    // new content's length. Emits one document event for the whole change.
    virtual bool replaceMidiClipContent(
        int32_t trackIndex,
        int32_t clipId,
        std::vector<uapmd_ump_t> umpEvents,
        std::vector<uint64_t> umpTickTimestamps,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Detached clip representation, shared by undo and the clipboard.
    //
    // Capture is non-destructive: deleting is a separate removeClipFromTrack,
    // which lets copy, cut and delete-with-undo all be composed from the same
    // two operations.
    //
    // Must NOT be called inside a document transaction. Extensions contribute
    // their own state to the fragment, and an ARA plug-in cannot be archived
    // while its document is being edited -- which is precisely what a
    // transaction holds open. Compose a cut as capture first, then remove
    // inside a transaction, rather than wrapping both.
    virtual std::optional<ProjectClipFragment> captureClipFragment(
        int32_t trackIndex,
        int32_t clipId) const = 0;

    // Recreates a captured clip on the given track. With
    // ProjectObjectIdPolicy::Restore the clip returns under its original
    // identifiers, which is what undoing a delete requires; with Mint it
    // becomes a new clip, which is what paste and duplicate require.
    virtual ClipAddResult attachClipFragment(
        int32_t trackIndex,
        const ProjectClipFragment& fragment,
        ProjectObjectIdPolicy idPolicy) = 0;

    // Detached track representation.
    //
    // Both halves are asynchronous, unlike the clip equivalents, because a
    // track owns live plugin instances: reading plugin state and creating
    // plugin instances are both callback-based. The callback is invoked
    // exactly once, on the thread that completes the last plugin operation.
    //
    // Capture, like the clip version, must not be called inside a document
    // transaction, and is non-destructive.
    using TrackFragmentCallback =
        std::function<void(std::optional<ProjectTrackFragment> fragment, std::string error)>;
    virtual void captureTrackFragment(int32_t trackIndex, TrackFragmentCallback callback) = 0;

    // Creates a new track from a captured one. `trackIndex` in the callback is
    // the created track, or -1 on failure.
    using TrackAttachCallback = std::function<void(int32_t trackIndex, std::string error)>;
    virtual void attachTrackFragment(
        const ProjectTrackFragment& fragment,
        ProjectTrackAttachOptions options,
        TrackAttachCallback callback) = 0;

    // Structural track mutations use the same asynchronous callback shape as
    // fragment attachment because deletion first captures plugin state.
    virtual void addEmptyTrack(
        ProjectMutationOrigin origin,
        TrackAttachCallback callback) = 0;
    virtual void removeTrack(
        int32_t trackIndex,
        ProjectMutationOrigin origin,
        TrackAttachCallback callback) = 0;

    // Persistent mixer properties. These address tracks by their stable
    // document identity during replay, so inserting or removing another track
    // does not redirect an undo operation to the wrong track.
    virtual bool setTrackGain(
        int32_t trackIndex,
        double gain,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackMuted(
        int32_t trackIndex,
        bool muted,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackSolo(
        int32_t trackIndex,
        bool solo,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackBypassed(
        int32_t trackIndex,
        bool bypassed,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackFreezePolicyEnabled(
        int32_t trackIndex,
        bool enabled,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Applies the complete persisted latency/monitoring configuration as one
    // history item. Callers that edit only one field should copy
    // latencyCompensationManager()->projectSettings(), alter that field, and
    // submit the resulting snapshot here.
    virtual bool setLatencyCompensationSettings(
        const LatencyCompensationProjectSettings& settings,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual bool addDeviceInputToTrack(
        int32_t trackIndex,
        int32_t sourceNodeId,
        const std::vector<uint32_t>& channelIndices,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setDeviceInputChannels(
        int32_t trackIndex,
        int32_t sourceNodeId,
        const std::vector<uint32_t>& channelIndices,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool removeDeviceInputFromTrack(
        int32_t trackIndex,
        int32_t sourceNodeId,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual bool setPluginBypassed(
        int32_t instanceId,
        bool bypassed,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setPluginParameterValue(
        int32_t instanceId,
        int32_t parameterIndex,
        double value,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setPluginGroup(
        int32_t instanceId,
        uint8_t group,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual void setPluginState(
        int32_t instanceId,
        std::vector<uint8_t> state,
        ProjectMutationOrigin origin,
        ProjectUndoCompletion completion) = 0;
    virtual bool connectTrackGraph(
        int32_t trackIndex,
        const uapmd_graph::AudioPluginGraphConnection& connection,
        std::string& error,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool disconnectTrackGraphConnection(
        int32_t trackIndex,
        int64_t connectionId,
        std::string& error,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    virtual bool appendMidiEventsToClip(int32_t trackIndex, int32_t clipId,
        std::vector<uapmd_ump_t> words, std::vector<uint64_t> ticks) = 0;

    // Project loading (synchronous; blocks until all plugin instantiations complete)
    struct ProjectResult {
        bool success{false};
        std::string error;
        std::vector<ClipMarker> masterTrackMarkers;
    };
    struct ProjectSaveOptions {
        std::vector<int32_t> excludedTrackIndexes;
        std::vector<ClipMarker> masterTrackMarkers;
        // Internal snapshots (for example track-freeze renders) must not look like
        // a user project save to document observers.
        bool emitDocumentEvent{true};
    };
    using ProjectSaveCallback = std::function<void(ProjectResult)>;
    using ProjectLoadCallback = std::function<void(ProjectResult)>;

    virtual void saveProject(
        const std::filesystem::path& file,
        ProjectSaveOptions options,
        ProjectSaveCallback callback) = 0;
    virtual void loadProject(const std::filesystem::path& file, ProjectLoadCallback callback) = 0;
    virtual AudioGraphProviderRegistry& audioGraphProviderRegistry() = 0;
    virtual const AudioGraphProviderRegistry& audioGraphProviderRegistry() const = 0;
    // Groups the document events produced by everything between the two calls
    // into a single batch, delivered when the outermost transaction ends.
    // Prefer the scoped form below. Calls nest.
    //
    // Use this whenever one user-visible action performs several mutations:
    // without it each mutation is delivered separately and observers can see
    // the action half-applied.
    virtual void beginDocumentTransaction() = 0;
    virtual void endDocumentTransaction() = 0;

    virtual ProjectDocumentEventSource& projectDocumentEvents() = 0;
    virtual ProjectDocumentView& projectDocumentView() = 0;
    virtual ProjectUndoEngine& undoEngine() = 0;
    virtual AudioSourceRepository& audioSourceRepository() = 0;
    virtual void setAudioSourceRepository(std::shared_ptr<AudioSourceRepository> repository) = 0;
    virtual bool replaceTrackGraphType(
        int32_t trackIndex,
        const std::string& graphTypeId,
        size_t eventBufferSizeInBytes,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool materializeProjectGraph(
        UapmdProjectTrackData* projectTrack,
        SequencerTrack* sequencerTrack,
        size_t eventBufferSizeInBytes) = 0;
    virtual bool saveProjectGraph(
        UapmdProjectTrackData* projectTrack,
        SequencerTrack* sequencerTrack,
        const std::filesystem::path& projectDir,
        const std::filesystem::path& graphDir,
        const std::string& scopeLabel,
        std::string& error) = 0;
    // The timeline does not own registered extensions. Callers must unregister an
    // extension before destroying it.
    virtual void addProjectSerializationExtension(ProjectSerializationExtension& extension) = 0;
    virtual void removeProjectSerializationExtension(ProjectSerializationExtension& extension) = 0;
    virtual bool saveProjectDataExtensions(
        UapmdProjectData& project,
        std::string& error) = 0;
    virtual bool loadProjectDataExtensions(
        UapmdProjectData& project,
        std::string& error) = 0;
    virtual bool saveProjectExtensionData(
        const std::filesystem::path& projectFile,
        const std::filesystem::path& projectDir,
        std::string& error) = 0;
    virtual bool loadProjectExtensionData(
        const std::filesystem::path& projectFile,
        const std::filesystem::path& projectDir,
        std::string& error) = 0;

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
    virtual ContentBounds calculateTrackContentBounds(int32_t trackIndex) const = 0;

    // Preview note data for a MIDI clip. Returns empty optional if track/clip not found or not MIDI.
    struct MidiNotePreview {
        double startSeconds{0.0};
        double durationSeconds{0.0};
        float  velocity{0.0f};   // 0.0–1.0
        uint8_t note{0};
    };
    virtual std::optional<std::vector<MidiNotePreview>> getMidiClipNotes(int32_t trackIndex, int32_t clipId) const = 0;

    // Fired on the calling thread after every clip add/remove and after loadProject() completes.
    // Pass nullptr to clear.
    virtual void setTimelineChangedCallback(std::function<void()> callback) = 0;

    virtual uint32_t maxTrackLatencyInSamples() = 0;
    virtual uint32_t trackRenderOffsetInSamples(int32_t trackIndex) = 0;
    virtual uint32_t masterTrackRenderOffsetInSamples() = 0;
    virtual bool trackHasLiveInput(int32_t trackIndex) = 0;

    // Audio preprocess callback — feeds clip source nodes into track input buffers.
    // Called by SequencerEngineImpl via the registered AudioPreprocessCallback.
    // Writes into targetSequence.tracks[i], typically pump ring-buffer slots.
    virtual void processTracksAudio(AudioProcessContext& process, SequenceProcessContext& targetSequence) = 0;

    // Lifecycle hooks called by SequencerEngineImpl when tracks are added/removed
    virtual void onTrackAdded(uint32_t outputChannels,
                              double sampleRate,
                              uint32_t bufferSizeInFrames,
                              int32_t insertionIndex) = 0;
    virtual void onTrackRemoved(size_t trackIndex) = 0;
    virtual void onTrackGraphChanged(int32_t trackIndex) = 0;

    static std::unique_ptr<TimelineFacade> create(SequencerEngine& engine);
};

// Scoped form of TimelineFacade::beginDocumentTransaction(). Declare one for
// the whole of a multi-step edit so that observers see it applied at once.
class ScopedDocumentTransaction {
    TimelineFacade* facade_;

public:
    explicit ScopedDocumentTransaction(TimelineFacade& facade)
        : facade_(&facade) {
        facade_->beginDocumentTransaction();
    }

    ~ScopedDocumentTransaction() {
        facade_->endDocumentTransaction();
    }

    ScopedDocumentTransaction(const ScopedDocumentTransaction&) = delete;
    ScopedDocumentTransaction& operator=(const ScopedDocumentTransaction&) = delete;
};

} // namespace uapmd
