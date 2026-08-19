#pragma once

// The concrete TimelineFacade. Declared here rather than inside a single
// translation unit so that its implementation can be split by role:
// project I/O, audio rendering, plug-ins, fragments and the document view
// each live in their own file.
//
// Private to the timeline facade implementation.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <remidy/detail/event-loop.hpp>
#include "ProjectSerialization.hpp"
#include "TimelineHistoryValues.hpp"
#include "TimelineProjectStaging.hpp"
#include "TimelineProjectSerializer.hpp"
#include "TimelineProperties.hpp"
#include "ProjectCommandsImpl.hpp"
#include "TimelineUndoOperations.hpp"
#include "remidy/remidy.hpp"
#include "uapmd-data/uapmd-data.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"
#include "uapmd-engine/uapmd-engine.hpp"
#include <umppi/umppi.hpp>

namespace uapmd {

    using namespace timeline_detail;
    using namespace uapmd_plugin_hosting;

    class TimelineFacadeImpl : public TimelineFacade,
                               public ProjectDocumentView,
                               public ProjectAddressBook,
                               public PropertyCommandTarget,
                               public timeline_detail::TimelineProjectSerializerHost,
                               public PluginInstanceLifecycleListener {
        SequencerEngine& engine_;
        int32_t sampleRate_;
        uint32_t bufferSizeInFrames_;

        using TrackList = std::vector<std::shared_ptr<TimelineTrack>>;
        TrackList timeline_tracks_;                          // UI-thread owned
        RtSnapshotPublisher<TrackList> timeline_tracks_snapshot_;

        // The tempo/time-signature map the audio callback reads. Building it
        // walks and sorts every master clip, which allocates, so it is built
        // on the model thread whenever master content changes and published
        // here for the RT thread to load -- the same arrangement as
        // timeline_tracks_snapshot_ above. Never null.
        RtSnapshotPublisher<MasterTrackSnapshot> master_track_snapshot_;
        std::shared_ptr<TimelineTrack> master_timeline_track_;

        // Propagates the master track's tempo/time-signature authority to every regular-track
        // MIDI clip (see MidiClipReader::applyAuthoritativeTempoMapToMusicalClips).
        void applyAuthoritativeTempoMapToMusicalClips();

        void resolveAllClipAnchors();

        static void appendMidiNodeMetaToSnapshot(MasterTrackSnapshot& snapshot,
                                                 const ClipData& clip,
                                                 MidiClipSourceNode& midiNode,
                                                 double sampleRate);

        void rebuildTrackSnapshot();
        // Walks the master track's MIDI clips. Model thread only.
        MasterTrackSnapshot computeMasterTrackSnapshot() const;
        // Recomputes and publishes for the audio thread. Model thread only.
        void rebuildMasterTrackSnapshot();

        TimelineState timeline_;
        int32_t next_source_node_id_{1};
        uint32_t next_timeline_track_reference_{1};
        std::function<void()> timeline_changed_callback_{};
        // Detail for the most recent failed property write; see
        // PropertyCommandTarget::lastWriteFailure().
        std::string last_write_failure_{};
        bool suppress_timeline_notification_{false};
        bool suppress_project_document_events_{false};
        AudioGraphProviderRegistry audio_graph_provider_registry_{};
        ProjectDocumentEventDispatcher project_document_events_{};
        ProjectUndoEngine undo_engine_{{
            .maximumHistorySizeInBytes = 64u * 1024u * 1024u,
            .dispatchToModelThread = [](ProjectUndoTask task) {
                if (!task)
                    return;
                if (remidy::EventLoop::runningOnMainThread())
                    task();
                else
                    remidy::EventLoop::enqueueTaskOnMainThread(std::move(task));
            }
        }};
        // References undo_engine_ rather than owning a history of its own:
        // operations that have not become commands yet still record into the
        // same history, and two histories would silently diverge.
        ProjectCommandManager command_manager_{{
            .history = &undo_engine_,
            .dispatchToModelThread = [](ProjectUndoTask task) {
                dispatchToModelThread(std::move(task));
            },
            .beginDocumentTransaction = [this] {
                project_document_events_.beginTransaction();
            },
            .endDocumentTransaction = [this] {
                project_document_events_.endTransaction();
            }
        }};
        timeline_detail::TimelineProjectSerializer serializer_{engine_, *this, *this};
        ProjectCommandsImpl commands_{engine_, *this, command_manager_};
        std::shared_ptr<AudioSourceRepository> audio_source_repository_{std::make_shared<FileAudioSourceRepository>()};
        mutable std::mutex project_serialization_extensions_mutex_{};
        std::vector<ProjectSerializationExtension*> project_serialization_extensions_{};
        std::unordered_map<int32_t, std::unordered_map<int32_t, double>> plugin_parameter_values_{};
        std::unordered_map<int32_t, remidy::EventListenerId> plugin_parameter_listener_ids_{};
        std::unordered_map<int32_t, std::vector<uint8_t>> plugin_state_values_{};
        std::unordered_set<int32_t> pending_plugin_state_captures_{};
        remidy::EventListenerId plugin_state_change_listener_id_{0};
        std::atomic<uint32_t> pending_plugin_mutations_{0};
        uint32_t suppress_plugin_graph_notifications_{0};

        // Identity staged by the project loader for the next track or clip it
        // creates, so that loaded objects keep the reference IDs they were
        // saved under instead of being given freshly allocated ones. Empty
        // means "allocate as usual"; each is consumed by the next creation.
        std::string pending_track_reference_id_{};
        std::string pending_clip_reference_id_{};

        // Copied under the lock so that extensions can be invoked without
        // holding it; an extension may register or unregister another.
        std::vector<ProjectSerializationExtension*> projectSerializationExtensionsSnapshot() const {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            return project_serialization_extensions_;
        }

        // Resolves a track index to its TimelineTrack, mapping
        // kMasterTrackIndex onto the master track. Returns nullptr when the
        // index addresses no track.
        TimelineTrack* resolveTrack(int32_t trackIndex) {
            if (trackIndex == kMasterTrackIndex)
                return master_timeline_track_.get();
            if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size()))
                return timeline_tracks_[static_cast<size_t>(trackIndex)].get();
            return nullptr;
        }

        const TimelineTrack* resolveTrack(int32_t trackIndex) const {
            return const_cast<TimelineFacadeImpl*>(this)->resolveTrack(trackIndex);
        }

        TimelineTrack* resolveTrackByReferenceId(std::string_view trackReferenceId);

        static int32_t clipIdForReferenceId(
            const TimelineTrack& track,
            std::string_view clipReferenceId);

        int32_t trackIndexForPersistentId(std::string_view trackReferenceId) const;

        bool removeClipRaw(TimelineTrack& targetTrack, int32_t clipId);

        bool removeClipByReferenceId(
            std::string_view trackReferenceId,
            std::string_view clipReferenceId);

        ClipAddResult restoreClipByReferenceId(
            std::string_view trackReferenceId,
            const ProjectClipFragment& fragment);

        bool replaceClipByReferenceId(
            std::string_view trackReferenceId,
            const ProjectClipFragment& desired,
            const ProjectClipFragment* compensation);

        bool recordReplacedClip(
            int32_t trackIndex,
            ProjectClipFragment before,
            ProjectMutationOrigin origin,
            std::string description);

        bool performCapturedClipRemoval(
            std::string trackReferenceId,
            ProjectClipFragment fragment,
            ProjectMutationOrigin origin);

        ClipAddResult recordAddedClip(
            int32_t trackIndex,
            ClipAddResult result,
            ProjectMutationOrigin origin);

        static void dispatchToModelThread(ProjectUndoTask task) {
            if (!task)
                return;
            if (remidy::EventLoop::runningOnMainThread())
                task();
            else
                remidy::EventLoop::enqueueTaskOnMainThread(std::move(task));
        }

        std::shared_ptr<ProjectUndoableOperation> makeTrackStructureOperation(
            TrackStructureUndoOperation::InitialDirection initialDirection,
            int32_t insertionIndex,
            ProjectTrackFragment fragment);


        SequencerTrack* resolveSequencerTrackByReferenceId(
            std::string_view trackReferenceId);

        void emitTrackChanged(
            std::string_view trackReferenceId,
            std::string changeType);


        std::string takePendingClipReferenceId() {
            auto id = std::move(pending_clip_reference_id_);
            // A moved-from std::string is valid but unspecified, and short
            // strings are commonly left intact. Clear explicitly so a staged
            // identity cannot be handed to a second clip.
            pending_clip_reference_id_.clear();
            return id;
        }

        // Keeps the allocator ahead of every restored identifier, so that a
        // track added after a load cannot collide with one the project already
        // used.
        void reserveTrackReferenceId(std::string_view referenceId) {
            constexpr std::string_view prefix{"track_"};
            if (!referenceId.starts_with(prefix))
                return;
            uint32_t value{};
            auto digits = referenceId.substr(prefix.size());
            auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
            if (ec != std::errc{} || ptr != digits.data() + digits.size())
                return;
            if (value >= next_timeline_track_reference_)
                next_timeline_track_reference_ = value + 1;
        }

        // TimelineProjectSerializerHost -- the timeline-private state a
        // project read or write needs. Everything else the serializer uses
        // goes through the public TimelineFacade surface.

        double sampleRate() const override {
            return static_cast<double>(sampleRate_);
        }

        uint32_t bufferSizeInFrames() const override {
            return bufferSizeInFrames_;
        }

        std::vector<ProjectSerializationExtension*> serializationExtensions() const override {
            return projectSerializationExtensionsSnapshot();
        }

        void stageTrackReferenceId(std::string referenceId) override {
            pending_track_reference_id_ = std::move(referenceId);
        }

        void stageClipReferenceId(std::string referenceId) override {
            pending_clip_reference_id_ = std::move(referenceId);
        }

        void resetTrackReferenceAllocator() override {
            next_timeline_track_reference_ = 1;
        }

        void setLoadInProgress(bool value) override {
            suppress_timeline_notification_ = value;
            suppress_project_document_events_ = value;
        }

        std::vector<ClipMarker> masterTrackMarkers() const override {
            return engine_.masterTrackMarkers();
        }

        bool applyMasterTrackMarkers(std::vector<ClipMarker> markers) override {
            engine_.setMasterTrackMarkers(std::move(markers));
            resolveAllClipAnchors();
            engine_.markTrackDirty(kMasterTrackIndex);
            return true;
        }

        LatencyCompensationProjectSettings latencyCompensationSettings() const override {
            auto* manager = engine_.latencyCompensationManager();
            return manager ? manager->projectSettings() : LatencyCompensationProjectSettings{};
        }

        bool applyLatencyCompensationSettings(
            const LatencyCompensationProjectSettings& settings) override {
            last_write_failure_.clear();
            auto* manager = engine_.latencyCompensationManager();
            if (!manager) {
                last_write_failure_ = "There is no latency compensation manager.";
                return false;
            }
            if (!manager->applyProjectSettings(settings, last_write_failure_))
                return false;
            notifyTimelineChanged();
            return true;
        }

        std::optional<std::vector<uint32_t>> deviceInputChannels(
            std::string_view trackReferenceId,
            int32_t sourceNodeId) override;

        bool applyDeviceInputChannels(
            std::string_view trackReferenceId,
            int32_t sourceNodeId,
            const std::optional<std::vector<uint32_t>>& channels) override {
            return applyDeviceInputState(trackReferenceId, sourceNodeId, channels);
        }

        std::optional<uapmd_graph::AudioPluginGraphConnection>
        graphConnectionById(int32_t trackIndex, int64_t connectionId) override;

        bool graphConnectionPresent(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& connection) override;

        bool applyGraphConnectionPresence(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& connection,
            bool present) override {
            last_write_failure_.clear();
            return applyGraphConnectionState(
                trackReferenceId, connection, present, last_write_failure_);
        }

        std::optional<std::string> resolveGraphProviderId(
            const std::string& graphTypeId) override {
            auto* provider = audio_graph_provider_registry_.get(graphTypeId);
            if (!provider)
                return std::nullopt;
            return provider->id();
        }

        std::optional<TrackGraphSnapshot> captureTrackGraph(
            std::string_view trackReferenceId) override {
            return captureTrackGraphSnapshot(
                trackIndexForPersistentId(trackReferenceId));
        }

        bool applyTrackGraph(
            std::string_view trackReferenceId,
            const TrackGraphSnapshot& snapshot,
            size_t eventBufferSizeInBytes) override {
            return applyTrackGraphSnapshot(
                trackReferenceId, snapshot, eventBufferSizeInBytes);
        }

        std::string lastWriteFailure() const override {
            return last_write_failure_;
        }


        void replaceMasterTimelineTrack(std::shared_ptr<TimelineTrack> track) override {
            master_timeline_track_ = std::move(track);
        }

        ClipAddResult addAudioClipToTimelineTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps) override {
            return addAudioClipToTrack(
                timelineTrack,
                position,
                std::move(reader),
                filepath,
                std::move(markers),
                std::move(audioWarps));
        }

    public:
        explicit TimelineFacadeImpl(SequencerEngine& engine)
            : engine_(engine)
            , sampleRate_(0)
            , bufferSizeInFrames_(0)
            , master_timeline_track_(std::make_shared<TimelineTrack>(std::string("master_track"), 0, 48000.0, 0))
        {
            engine_.addPluginInstanceLifecycleListener(*this);
            if (auto* pluginHost = engine_.pluginHost())
                plugin_state_change_listener_id_ =
                    pluginHost->addPluginStateChangeListener(
                        [this](int32_t instanceId) {
                            onPluginStateChanged(instanceId);
                        });
            audio_graph_provider_registry_ = AudioGraphProviderRegistry::create();
            timeline_.tempo = 120.0;
            timeline_.timeSignatureNumerator = 4;
            timeline_.timeSignatureDenominator = 4;
            timeline_.isPlaying = false;
            timeline_.loopEnabled = false;
        }

        ~TimelineFacadeImpl() override {
            if (plugin_state_change_listener_id_ != 0) {
                if (auto* pluginHost = engine_.pluginHost())
                    pluginHost->removePluginStateChangeListener(
                        plugin_state_change_listener_id_);
            }
            engine_.removePluginInstanceLifecycleListener(*this);
            for (const auto& [instanceId, listenerId] : plugin_parameter_listener_ids_) {
                if (auto* instance = engine_.getPluginInstance(instanceId)) {
                    if (auto* support = instance->parameterSupport())
                        support->parameterChangeEvent().removeListener(listenerId);
                }
            }
        }

        void notifyTimelineChanged() {
            if (!suppress_timeline_notification_ && timeline_changed_callback_)
                timeline_changed_callback_();
        }

        void emitProjectDocumentEvent(ProjectDocumentEvent event) {
            if (!suppress_project_document_events_)
                project_document_events_.emit(std::move(event));
        }

        static std::string clipObjectId(const TimelineTrack& track, const ClipData* clip, int32_t clipId);

        static std::string audioSourceObjectId(const TimelineTrack& track, const ClipData& clip);

        size_t audioSourceReferenceCount(const std::string& audioSourceId) const;

        int32_t trackIndexFor(const TimelineTrack& track) const;

        TimelineTrack* findTrackById(const ProjectObjectId& trackId) const;

        std::optional<std::pair<TimelineTrack*, ClipData>> findClipById(const ProjectObjectId& clipId) const;

        ProjectClipSnapshot makeClipSnapshot(const TimelineTrack& track, const ClipData& clip) const;

        void emitClipAdded(TimelineTrack& track, int32_t clipId, int32_t sourceNodeId);

        void emitClipRemoved(TimelineTrack& track, const ClipData& clip);

        void emitClipChanged(TimelineTrack& track, const ClipData& clip, std::string type);

        void emitMasterTrackChanged(std::string type = "master-track-changed");

        // ---- TimelineFacade interface ----

        TimelineState& state() override { return timeline_; }

        std::vector<TimelineTrack*> tracks() override {
            std::vector<TimelineTrack*> result;
            result.reserve(timeline_tracks_.size());
            for (auto& t : timeline_tracks_)
                result.push_back(t.get());
            return result;
        }

        TimelineTrack* masterTimelineTrack() override {
            return master_timeline_track_.get();
        }

        int32_t trackIndexForReferenceId(std::string_view trackId) const override {
            return trackIndexForPersistentId(trackId);
        }

        // ProjectAddressBook -- the one place that translates between the
        // persistent identities a command carries and the runtime indexes and
        // pointers the engine works with.

        ProjectAddressBook& addresses() override {
            return *this;
        }

        ProjectCommands& commands() override {
            return commands_;
        }

        TimelineTrack* timelineTrack(std::string_view trackReferenceId) override;

        SequencerTrack* sequencerTrack(std::string_view trackReferenceId) override;

        int32_t trackIndex(std::string_view trackReferenceId) const override;

        int32_t clipId(const ClipAddress& address) const override;

        int32_t pluginInstanceId(const PluginAddress& address) override;

        std::optional<ProjectObjectId> trackReferenceId(int32_t index) const override;

        std::optional<ClipAddress> clipAddress(
            int32_t index,
            int32_t clipIdentifier) const override;

        std::optional<PluginAddress> pluginAddress(int32_t instanceId) override;

        // PropertyCommandTarget

        double timelineSampleRate() const override {
            return static_cast<double>(sampleRate_);
        }

        void onClipMutated(
            TimelineTrack& track,
            int32_t clipIdentifier,
            std::string_view changeType) override;

        void onTrackChanged(
            std::string_view trackReference,
            std::string_view changeType) override;

        void onTrackMutated(
            std::string_view trackReference,
            std::string_view changeType) override;

        void resolveClipAnchors() override;

        AudioPluginInstanceAPI* pluginInstance(int32_t instanceId) override;

        bool pluginInstanceBusy(int32_t instanceId) override;

        uint8_t pluginInstanceGroup(int32_t instanceId) override;

        bool setPluginInstanceGroup(int32_t instanceId, uint8_t group) override;

        bool applyPluginParameter(
            std::string_view trackReference,
            std::string_view nodeId,
            int32_t parameterIndex,
            double value) override;

        bool setTrackFreezePolicy(int32_t index, bool enabled) override;

        bool trackFreezePolicyEnabled(int32_t index) const override;

        AudioGraphProviderRegistry& audioGraphProviderRegistry() override {
            return audio_graph_provider_registry_;
        }

        const AudioGraphProviderRegistry& audioGraphProviderRegistry() const override {
            return audio_graph_provider_registry_;
        }

        SequencerTrack* resolveSequencerTrack(int32_t trackIndex) {
            if (trackIndex == kMasterTrackIndex)
                return engine_.masterTrack();
            auto& tracks = engine_.tracks();
            if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()))
                return tracks[static_cast<size_t>(trackIndex)];
            return nullptr;
        }

        void captureTrackFragment(int32_t trackIndex, TrackFragmentCallback callback) override;

        void attachTrackFragment(
            const ProjectTrackFragment& fragment,
            ProjectTrackAttachOptions options,
            TrackAttachCallback callback) override;

        void addEmptyTrack(
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) override;

        void recordTrackAddition(
            int32_t trackIndex,
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) override;

        void removeTrack(
            int32_t trackIndex,
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) override;

        void beginDocumentTransaction() override {
            project_document_events_.beginTransaction();
        }

        void endDocumentTransaction() override {
            project_document_events_.endTransaction();
        }

        ProjectDocumentEventSource& projectDocumentEvents() override {
            return project_document_events_;
        }

        ProjectUndoEngine& undoEngine() override {
            return undo_engine_;
        }

        ProjectDocumentView& projectDocumentView() override {
            return *this;
        }

        AudioSourceRepository& audioSourceRepository() override {
            return *audio_source_repository_;
        }

        void setAudioSourceRepository(std::shared_ptr<AudioSourceRepository> repository) override {
            if (repository)
                audio_source_repository_ = std::move(repository);
            else
                audio_source_repository_ = std::make_shared<FileAudioSourceRepository>();
        }

        ProjectRevision currentRevision() const override;

        std::optional<ProjectObjectId> masterTrackId() const override;

        std::vector<ProjectObjectId> trackIds() const override;

        std::vector<ProjectObjectId> clipIds(ProjectObjectId trackId) const override;

        std::vector<ProjectObjectId> audioSourceIds() const override;

        std::optional<ProjectTrackSnapshot> getTrack(ProjectObjectId trackId) const override;

        std::optional<ProjectClipSnapshot> getClip(ProjectObjectId clipId) const override;

        std::optional<ProjectAudioSourceSnapshot> getAudioSource(ProjectObjectId audioSourceId) const override;

        bool readClipUmpContent(
            ProjectObjectId clipId,
            std::vector<uapmd_ump_t>& events,
            std::vector<uint64_t>& timestampsInTicks,
            uint32_t& tickResolution) const override;

        bool readAudioSourceSamples(
            ProjectObjectId audioSourceId,
            int64_t startFrame,
            int64_t frameCount,
            float** destination,
            uint32_t destinationChannels) const override;

        std::optional<TrackGraphSnapshot> captureTrackGraphSnapshot(
            int32_t trackIndex);

        bool applyTrackGraphSnapshot(
            std::string_view trackReferenceId,
            const TrackGraphSnapshot& snapshot,
            size_t eventBufferSizeInBytes);


        bool materializeProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            size_t eventBufferSizeInBytes) override {
            return serializer_.materializeProjectGraph(projectTrack, sequencerTrack, eventBufferSizeInBytes);
        }

        bool saveProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            const std::filesystem::path& projectDir,
            const std::filesystem::path& graphDir,
            const std::string& scopeLabel,
            std::string& error) override {
            return serializer_.saveProjectGraph(
                projectTrack, sequencerTrack, projectDir, graphDir, scopeLabel, error);
        }

        void addProjectSerializationExtension(ProjectSerializationExtension& extension) override {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            if (std::find(project_serialization_extensions_.begin(), project_serialization_extensions_.end(), &extension) ==
                project_serialization_extensions_.end())
                project_serialization_extensions_.push_back(&extension);
        }

        void removeProjectSerializationExtension(ProjectSerializationExtension& extension) override {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            std::erase(project_serialization_extensions_, &extension);
        }

        bool saveProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) override {
            return serializer_.saveProjectDataExtensions(project, error);
        }

        bool loadProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) override {
            return serializer_.loadProjectDataExtensions(project, error);
        }

        bool saveProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) override {
            return serializer_.saveProjectExtensionData(projectFile, projectDir, error);
        }

        bool loadProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) override {
            return serializer_.loadProjectExtensionData(projectFile, projectDir, error);
        }


        void saveProject(
            const std::filesystem::path& projectFile,
            ProjectSaveOptions options,
            ProjectSaveCallback callback) override {
            serializer_.saveProject(projectFile, std::move(options), std::move(callback));
        }

        ClipAddResult addMidiClipToTimelineTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            const std::string& filepath,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName,
            bool nrpnToParameterMapping,
            bool needsFileSave,
            int32_t requestedClipId = -1);

        ClipAddResult addAudioClipToTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps,
            int32_t requestedClipId = -1) ;

        ClipAddResult addAudioClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            ProjectMutationOrigin origin) override;

        ClipAddResult addMidiClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            const std::string& filepath,
            bool nrpnToParameterMapping,
            ProjectMutationOrigin origin) override;

        ClipAddResult addMidiClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName,
            bool nrpnToParameterMapping,
            bool needsFileSave,
            ProjectMutationOrigin origin) override;

        ClipAddResult addMasterMidiClip(
            const TimelinePosition& position,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName,
            bool needsFileSave,
            const std::string& filepath,
            ProjectMutationOrigin origin) override;

        bool removeClipFromTrack(
            int32_t trackIndex,
            int32_t clipId,
            ProjectMutationOrigin origin) override;

        bool clearClipsFromTrack(
            int32_t trackIndex,
            ProjectMutationOrigin origin) override;







        bool applyDeviceInputState(
            std::string_view trackReferenceId,
            int32_t sourceNodeId,
            const std::optional<std::vector<uint32_t>>& channels);

        // A plug-in's document identity is its (track, node) address; the
        // runtime instance id is regenerated whenever it is restored.
        using PluginTarget = PluginAddress;

        std::optional<PluginTarget> pluginTargetForInstance(
            int32_t instanceId);

        int32_t resolvePluginInstanceId(
            std::string_view trackReferenceId,
            std::string_view nodeId);

        bool applyPluginParameterValue(
            std::string_view persistentTrackId,
            std::string_view persistentNodeId,
            int32_t parameterIndex,
            double value);

        void onPluginParameterChanged(
            int32_t instanceId,
            uint32_t parameterIndex,
            double value);

        void refreshPluginParameterCache(int32_t instanceId);

        void onPluginStateChanged(int32_t instanceId);

        void pluginInstanceAdded(
            int32_t instanceId,
            AudioPluginInstanceAPI& instance) override {
            plugin_state_values_[instanceId] = instance.saveStateSync();
            auto& values = plugin_parameter_values_[instanceId];
            for (const auto& parameter : instance.parameterMetadataList())
                values[static_cast<int32_t>(parameter.index)] =
                    instance.getParameterValue(static_cast<int32_t>(parameter.index));
            auto* support = instance.parameterSupport();
            if (!support)
                return;
            plugin_parameter_listener_ids_[instanceId] =
                support->parameterChangeEvent().addListener(
                [this, instanceId](uint32_t parameterIndex, double value) {
                    onPluginParameterChanged(instanceId, parameterIndex, value);
                });
        }

        void pluginInstanceWillBeDestroyed(int32_t instanceId) override {
            const auto listener = plugin_parameter_listener_ids_.find(instanceId);
            if (listener != plugin_parameter_listener_ids_.end()) {
                if (auto* instance = engine_.getPluginInstance(instanceId)) {
                    if (auto* support = instance->parameterSupport())
                        support->parameterChangeEvent().removeListener(listener->second);
                }
                plugin_parameter_listener_ids_.erase(listener);
            }
            plugin_parameter_values_.erase(instanceId);
            plugin_state_values_.erase(instanceId);
            pending_plugin_state_captures_.erase(instanceId);
        }






        // Keeps hasPendingPluginMutations() true while an asynchronous plug-in
        // mutation is in flight, so that a project save waits for it.
        ProjectUndoCompletion trackPendingPluginMutation(ProjectUndoCompletion completion);

        // Refreshes the caches a plug-in mutation invalidates and reports the
        // change, on the model thread. Shared by the state and preset paths,
        // which previously spelled this sequence out four times between them.
        void finishPluginMutation(
            int32_t instanceId,
            std::string_view trackReferenceId,
            std::string error,
            ProjectUndoCompletion completion);

        void applyPluginState(
            std::string trackReferenceId,
            std::string nodeId,
            std::vector<uint8_t> state,
            ProjectUndoCompletion completion);

        // Loads a preset into the addressed plug-in and reports the change.
        // Both the initial load and its redo run through here.
        void applyPluginPreset(
            PluginAddress address,
            int32_t presetIndex,
            ProjectUndoCompletion completion);

        // Recreates a plug-in under its persistent node identity and restores
        // its opaque state. Both plug-in lifecycle directions need this: undo
        // of a removal and redo of an addition are the same act.
        void restorePluginInstance(
            std::string_view trackReferenceId,
            std::string_view format,
            std::string_view pluginId,
            std::string_view nodeId,
            bool bypassed,
            uint8_t group,
            const std::vector<uint8_t>& state,
            const std::vector<uapmd_graph::AudioPluginGraphConnection>& connections,
            std::function<void(int32_t, std::string)> finished);

        void removePluginInstanceById(
            int32_t instanceId,
            std::function<void(std::string)> finished);

        // Captures the plug-in's project state and records one lifecycle
        // history entry. For a removal the instance is removed once its state
        // has been captured; for an addition the instance already exists and
        // only the history entry is created.
        void recordPluginInstanceLifecycle(
            int32_t instanceId,
            bool isAddition,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion);

        void setPluginState(
            int32_t instanceId,
            std::vector<uint8_t> state,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) override;

        PluginStateUndoOperation::Apply pluginStateApplier();

        void loadPluginPreset(
            int32_t instanceId,
            int32_t presetIndex,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) override;

        void recordPluginInstanceAddition(
            int32_t instanceId,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) override;

        void removePluginInstance(
            int32_t instanceId,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) override;

        bool hasPendingPluginMutations() const override;

        static bool graphEndpointEquivalent(
            const uapmd_graph::AudioPluginGraphEndpoint& lhs,
            const uapmd_graph::AudioPluginGraphEndpoint& rhs) {
            return timeline_detail::graphEndpointEquivalent(lhs, rhs);
        }

        static bool graphConnectionEquivalent(
            const uapmd_graph::AudioPluginGraphConnection& lhs,
            const uapmd_graph::AudioPluginGraphConnection& rhs) {
            return timeline_detail::graphConnectionEquivalent(lhs, rhs);
        }

        bool applyGraphConnectionState(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& desired,
            bool present,
            std::string& error);



        bool notifyClipChanged(int32_t trackIndex, int32_t clipId, std::string type = "clip-changed") override;

        // Applies `mutate` to the addressed track's clip manager and, when it
        // reports a change, emits `changeType` and refreshes the timeline.
        // Every clip mutator goes through here so that no path can change a
        // clip without the matching document event being emitted.
        template<typename Mutation>
        bool mutateClip(int32_t trackIndex, int32_t clipId, std::string changeType, Mutation&& mutate) {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack || !mutate(targetTrack->clipManager()))
                return false;
            notifyClipChanged(trackIndex, clipId, std::move(changeType));
            notifyTimelineChanged();
            return true;
        }











        bool clipEnabled(int32_t trackIndex, int32_t clipId) const override;

        // Every clip in the project, addressable by reference identifier. Warp
        // and marker references may point at any clip, so resolution needs the
        // whole set.
        ClipReferenceMap buildClipReferenceMap() const;

        bool replaceAudioClipContent(
            int32_t trackIndex,
            int32_t clipId,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps,
            const std::vector<ClipMarker>& masterTrackMarkers,
            ProjectMutationOrigin origin) override;

        bool replaceMidiClipContent(
            int32_t trackIndex,
            int32_t clipId,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            ProjectMutationOrigin origin) override;

        std::optional<ProjectClipFragment> captureClipFragment(
            int32_t trackIndex,
            int32_t clipId) const override;

        ClipAddResult attachClipFragment(
            int32_t trackIndex,
            const ProjectClipFragment& fragment,
            ProjectObjectIdPolicy idPolicy) override;

        bool appendMidiEventsToClip(int32_t trackIndex, int32_t clipId,
            std::vector<uapmd_ump_t> words, std::vector<uint64_t> ticks) override;

        void loadProject(const std::filesystem::path& projectFile, ProjectLoadCallback callback) override {
            serializer_.loadProject(projectFile, std::move(callback));
        }

        MasterTrackSnapshot buildMasterTrackSnapshot() override;

        ContentBounds calculateTrackContentBounds(int32_t trackIndex) const override;

        ContentBounds calculateContentBounds() const override;

        void setTimelineChangedCallback(std::function<void()> cb) override {
            timeline_changed_callback_ = std::move(cb);
        }

        std::optional<std::vector<MidiNotePreview>> getMidiClipNotes(int32_t trackIndex, int32_t clipId) const override;

        uint32_t maxTrackLatencyInSamples() override;

        uint32_t trackRenderOffsetInSamples(int32_t trackIndex) override;

        uint32_t masterTrackRenderOffsetInSamples() override;

        bool trackHasLiveInput(int32_t trackIndex) override;

        void processTracksAudio(AudioProcessContext& process, SequenceProcessContext& targetSequence) override;

        void onTrackAdded(
            uint32_t outputChannels,
            double sampleRate,
            uint32_t bufferSizeInFrames,
            int32_t insertionIndex) override;

        void onTrackRemoved(size_t trackIndex) override;

        void onTrackGraphChanged(int32_t trackIndex) override;

    private:



    };

} // namespace uapmd
