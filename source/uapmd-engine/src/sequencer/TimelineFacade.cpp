#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#include <unordered_map>

#include <remidy/detail/event-loop.hpp>
#include "remidy/remidy.hpp"
#include "uapmd-data/uapmd-data.hpp"
#include "uapmd-engine/uapmd-engine.hpp"
#include <umppi/umppi.hpp>

namespace uapmd {

    class TimelineFacadeImpl : public TimelineFacade, public ProjectDocumentView {
        SequencerEngine& engine_;
        int32_t sampleRate_;
        uint32_t bufferSizeInFrames_;

        using TrackList = std::vector<std::shared_ptr<TimelineTrack>>;
        TrackList timeline_tracks_;                          // UI-thread owned
        std::shared_ptr<const TrackList> timeline_tracks_snapshot_; // RT-thread read via atomic
        std::shared_ptr<TimelineTrack> master_timeline_track_;

        static bool clipHasMeaningfulTempoMap(const MidiClipSourceNode& node) {
            const auto& tempoChanges = node.tempoChanges();
            if (tempoChanges.size() > 1)
                return true;
            if (!tempoChanges.empty() && std::abs(tempoChanges.front().bpm - 120.0) > 1.0e-6)
                return true;
            return false;
        }

        static bool clipHasMeaningfulTimeSignatureMap(const MidiClipSourceNode& node) {
            const auto& changes = node.timeSignatureChanges();
            if (changes.size() > 1)
                return true;
            if (!changes.empty() &&
                (changes.front().numerator != 4 || changes.front().denominator != 4))
                return true;
            return false;
        }

        struct TimelineMetaSource {
            ClipData clip;
            MidiClipSourceNode* node{nullptr};
        };

        std::optional<TimelineMetaSource> findAuthoritativeTimelineMetaSource(bool preferMasterTrack) const {
            auto findOnTrack = [this](const std::shared_ptr<TimelineTrack>& track) -> std::optional<TimelineMetaSource> {
                if (!track)
                    return std::nullopt;

                auto clips = track->clipManager().getAllClips();
                std::sort(clips.begin(), clips.end(), [](const ClipData& a, const ClipData& b) {
                    return a.clipId < b.clipId;
                });

                for (const auto& clip : clips) {
                    if (clip.clipType != ClipType::Midi)
                        continue;
                    auto sourceNode = track->getSourceNode(clip.sourceNodeInstanceId);
                    auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                    if (!midiNode)
                        continue;
                    if (clipHasMeaningfulTempoMap(*midiNode) || clipHasMeaningfulTimeSignatureMap(*midiNode))
                        return TimelineMetaSource{clip, midiNode};
                }
                return std::nullopt;
            };

            if (preferMasterTrack) {
                if (auto masterSource = findOnTrack(master_timeline_track_))
                    return masterSource;
            }

            for (const auto& track : timeline_tracks_) {
                if (auto source = findOnTrack(track))
                    return source;
            }

            if (!preferMasterTrack) {
                if (auto masterSource = findOnTrack(master_timeline_track_))
                    return masterSource;
            }

            return std::nullopt;
        }

        void applyAuthoritativeTempoMapToMusicalClips() {
            auto authoritative = findAuthoritativeTimelineMetaSource(true);
            for (const auto& track : timeline_tracks_) {
                if (!track)
                    continue;
                auto clips = track->clipManager().getAllClips();
                for (const auto& clip : clips) {
                    if (clip.clipType != ClipType::Midi)
                        continue;
                    auto sourceNode = track->getSourceNode(clip.sourceNodeInstanceId);
                    auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                    if (!midiNode)
                        continue;
                    if (authoritative && authoritative->node)
                        midiNode->setPlaybackTempoMap(authoritative->node->tempoChanges());
                    else
                        midiNode->clearPlaybackTempoMap();
                }
            }

            if (authoritative && authoritative->node && !authoritative->node->tempoChanges().empty())
                timeline_.tempo = authoritative->node->tempoChanges().front().bpm;
        }

        static void appendMidiNodeMetaToSnapshot(MasterTrackSnapshot& snapshot,
                                                 const ClipData& clip,
                                                 MidiClipSourceNode& midiNode,
                                                 double sampleRate) {
            const double clipStartSamples = static_cast<double>(clip.position.samples);

            const auto& tempoSamples = midiNode.tempoChangeSamples();
            const auto& tempoEvents = midiNode.tempoChanges();
            const size_t tempoCount = std::min(tempoSamples.size(), tempoEvents.size());
            for (size_t i = 0; i < tempoCount; ++i) {
                MasterTrackSnapshot::TempoPoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(tempoSamples[i])) / sampleRate;
                point.tickPosition = tempoEvents[i].tickPosition;
                point.bpm = tempoEvents[i].bpm;
                snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                snapshot.tempoPoints.push_back(point);
            }

            const auto& sigSamples = midiNode.timeSignatureChangeSamples();
            const auto& sigEvents = midiNode.timeSignatureChanges();
            const size_t sigCount = std::min(sigSamples.size(), sigEvents.size());
            for (size_t i = 0; i < sigCount; ++i) {
                MasterTrackSnapshot::TimeSignaturePoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(sigSamples[i])) / sampleRate;
                point.tickPosition = sigEvents[i].tickPosition;
                point.signature = sigEvents[i];
                snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                snapshot.timeSignaturePoints.push_back(point);
            }
        }

        void rebuildTrackSnapshot() {
            auto snap = std::make_shared<TrackList>(timeline_tracks_);
            std::atomic_store_explicit(&timeline_tracks_snapshot_,
                std::shared_ptr<const TrackList>(snap), std::memory_order_release);
        }

        TimelineState timeline_;
        int32_t next_source_node_id_{1};
        uint32_t next_timeline_track_reference_{1};
        std::function<void()> timeline_changed_callback_{};
        bool suppress_timeline_notification_{false};
        bool suppress_project_document_events_{false};
        AudioGraphProviderRegistry audio_graph_provider_registry_{};
        ProjectDocumentEventDispatcher project_document_events_{};
        std::shared_ptr<AudioSourceRepository> audio_source_repository_{std::make_shared<FileAudioSourceRepository>()};

    public:
        explicit TimelineFacadeImpl(SequencerEngine& engine)
            : engine_(engine)
            , sampleRate_(0)
            , bufferSizeInFrames_(0)
            , master_timeline_track_(std::make_shared<TimelineTrack>(std::string("master_track"), 0, 48000.0, 0))
        {
            audio_graph_provider_registry_ = AudioGraphProviderRegistry::create();
            timeline_.tempo = 120.0;
            timeline_.timeSignatureNumerator = 4;
            timeline_.timeSignatureDenominator = 4;
            timeline_.isPlaying = false;
            timeline_.loopEnabled = false;
        }

        void notifyTimelineChanged() {
            if (!suppress_timeline_notification_ && timeline_changed_callback_)
                timeline_changed_callback_();
        }

        void emitProjectDocumentEvent(ProjectDocumentEvent event) {
            if (!suppress_project_document_events_)
                project_document_events_.emit(std::move(event));
        }

        static std::string clipObjectId(const TimelineTrack& track, const ClipData* clip, int32_t clipId) {
            if (clip && !clip->referenceId.empty())
                return clip->referenceId;
            return std::format("{}::clip_{:08x}", track.referenceId(), static_cast<uint32_t>(clipId));
        }

        static std::string audioSourceObjectId(const TimelineTrack& track, const ClipData& clip) {
            if (!clip.filepath.empty())
                return "audio-source:" + clip.filepath;
            return "audio-source:" + clipObjectId(track, &clip, clip.clipId);
        }

        size_t audioSourceReferenceCount(const std::string& audioSourceId) const {
            auto countOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> size_t {
                if (!track)
                    return 0;
                size_t count = 0;
                for (const auto& clip : track->clipManager().getAllClips())
                    if (clip.clipType == ClipType::Audio && audioSourceObjectId(*track, clip) == audioSourceId)
                        ++count;
                return count;
            };

            size_t count = countOnTrack(master_timeline_track_);
            for (const auto& track : timeline_tracks_)
                count += countOnTrack(track);
            return count;
        }

        int32_t trackIndexFor(const TimelineTrack& track) const {
            if (&track == master_timeline_track_.get())
                return kMasterTrackIndex;
            for (int32_t i = 0; i < static_cast<int32_t>(timeline_tracks_.size()); ++i)
                if (timeline_tracks_[static_cast<size_t>(i)].get() == &track)
                    return i;
            return -1;
        }

        TimelineTrack* findTrackById(const ProjectObjectId& trackId) const {
            if (master_timeline_track_ && master_timeline_track_->referenceId() == trackId)
                return master_timeline_track_.get();
            for (const auto& track : timeline_tracks_)
                if (track && track->referenceId() == trackId)
                    return track.get();
            return nullptr;
        }

        std::optional<std::pair<TimelineTrack*, ClipData>> findClipById(const ProjectObjectId& clipId) const {
            auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> std::optional<std::pair<TimelineTrack*, ClipData>> {
                if (!track)
                    return std::nullopt;
                for (const auto& clip : track->clipManager().getAllClips())
                    if (clipObjectId(*track, &clip, clip.clipId) == clipId)
                        return std::make_pair(track.get(), clip);
                return std::nullopt;
            };

            if (auto found = findOnTrack(master_timeline_track_))
                return found;
            for (const auto& track : timeline_tracks_)
                if (auto found = findOnTrack(track))
                    return found;
            return std::nullopt;
        }

        ProjectClipSnapshot makeClipSnapshot(const TimelineTrack& track, const ClipData& clip) const {
            return ProjectClipSnapshot{
                .clipId = clipObjectId(track, &clip, clip.clipId),
                .trackId = track.referenceId(),
                .trackIndex = trackIndexFor(track),
                .clipNumericId = clip.clipId,
                .sourceNodeId = clip.sourceNodeInstanceId,
                .clipType = clip.clipType,
                .name = clip.name,
                .filepath = clip.filepath,
                .position = clip.position,
                .durationSamples = clip.durationSamples,
                .tickResolution = clip.tickResolution,
                .clipTempo = clip.clipTempo,
                .markers = clip.markers,
                .audioWarps = clip.audioWarps
            };
        }

        void emitClipAdded(TimelineTrack& track, int32_t clipId, int32_t sourceNodeId) {
            auto* clip = track.clipManager().getClip(clipId);
            ProjectDocumentEvent event(ProjectDocumentEventKind::ClipAdded, "clip-added");
            event.setTrackId(track.referenceId())
                .setClipId(clipObjectId(track, clip, clipId))
                .setTrackIndex(trackIndexFor(track))
                .setClipNumericId(clipId)
                .setDetail("source-node-id", static_cast<int64_t>(sourceNodeId));
            if (clip) {
                event.setDetail("clip-type", std::string(clip->clipType == ClipType::Audio ? "audio" : "midi"));
                if (!clip->filepath.empty())
                    event.setDetail("source.file", clip->filepath);
            }
            emitProjectDocumentEvent(std::move(event));

            if (clip && clip->clipType == ClipType::Audio) {
                const auto audioSourceId = audioSourceObjectId(track, *clip);
                if (audioSourceReferenceCount(audioSourceId) == 1) {
                    ProjectDocumentEvent sourceEvent(ProjectDocumentEventKind::AudioSourceAdded, "audio-source-added");
                    sourceEvent.setAudioSourceId(audioSourceId)
                        .setClipId(clipObjectId(track, clip, clipId))
                        .setDetail("source.file", clip->filepath);
                    emitProjectDocumentEvent(std::move(sourceEvent));
                }
            }
        }

        void emitClipRemoved(TimelineTrack& track, const ClipData& clip) {
            ProjectDocumentEvent event(ProjectDocumentEventKind::ClipRemoved, "clip-removed");
            event.setTrackId(track.referenceId())
                .setClipId(clipObjectId(track, &clip, clip.clipId))
                .setTrackIndex(trackIndexFor(track))
                .setClipNumericId(clip.clipId)
                .setDetail("source-node-id", static_cast<int64_t>(clip.sourceNodeInstanceId));
            emitProjectDocumentEvent(std::move(event));

            if (clip.clipType == ClipType::Audio) {
                const auto audioSourceId = audioSourceObjectId(track, clip);
                if (audioSourceReferenceCount(audioSourceId) == 0) {
                    ProjectDocumentEvent sourceEvent(ProjectDocumentEventKind::AudioSourceRemoved, "audio-source-removed");
                    sourceEvent.setAudioSourceId(audioSourceId)
                        .setClipId(clipObjectId(track, &clip, clip.clipId))
                        .setDetail("source.file", clip.filepath);
                    emitProjectDocumentEvent(std::move(sourceEvent));
                }
            }
        }

        void emitMasterTrackChanged(std::string type = "master-track-changed") {
            ProjectDocumentEvent event(ProjectDocumentEventKind::MasterTrackChanged, std::move(type));
            event.setTrackId(master_timeline_track_ ? master_timeline_track_->referenceId() : "master_track")
                .setTrackIndex(kMasterTrackIndex);
            emitProjectDocumentEvent(std::move(event));
        }

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

        AudioGraphProviderRegistry& audioGraphProviderRegistry() override {
            return audio_graph_provider_registry_;
        }

        const AudioGraphProviderRegistry& audioGraphProviderRegistry() const override {
            return audio_graph_provider_registry_;
        }

        ProjectDocumentEventSource& projectDocumentEvents() override {
            return project_document_events_;
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

        ProjectRevision currentRevision() const override {
            return project_document_events_.currentRevision();
        }

        std::optional<ProjectObjectId> masterTrackId() const override {
            if (!master_timeline_track_)
                return std::nullopt;
            return master_timeline_track_->referenceId();
        }

        std::vector<ProjectObjectId> trackIds() const override {
            std::vector<ProjectObjectId> result;
            result.reserve(timeline_tracks_.size());
            for (const auto& track : timeline_tracks_)
                if (track)
                    result.push_back(track->referenceId());
            return result;
        }

        std::vector<ProjectObjectId> clipIds(ProjectObjectId trackId) const override {
            std::vector<ProjectObjectId> result;
            auto* track = findTrackById(trackId);
            if (!track)
                return result;
            auto clips = track->clipManager().getAllClips();
            result.reserve(clips.size());
            for (const auto& clip : clips)
                result.push_back(clipObjectId(*track, &clip, clip.clipId));
            return result;
        }

        std::vector<ProjectObjectId> audioSourceIds() const override {
            std::vector<ProjectObjectId> result;
            auto collect = [&result](const std::shared_ptr<TimelineTrack>& track) {
                if (!track)
                    return;
                for (const auto& clip : track->clipManager().getAllClips())
                    if (clip.clipType == ClipType::Audio)
                        result.push_back(audioSourceObjectId(*track, clip));
            };
            collect(master_timeline_track_);
            for (const auto& track : timeline_tracks_)
                collect(track);
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        std::optional<ProjectTrackSnapshot> getTrack(ProjectObjectId trackId) const override {
            auto* track = findTrackById(trackId);
            if (!track)
                return std::nullopt;
            return ProjectTrackSnapshot{
                .trackId = track->referenceId(),
                .trackIndex = trackIndexFor(*track),
                .masterTrack = track == master_timeline_track_.get()
            };
        }

        std::optional<ProjectClipSnapshot> getClip(ProjectObjectId clipId) const override {
            auto found = findClipById(clipId);
            if (!found)
                return std::nullopt;
            return makeClipSnapshot(*found->first, found->second);
        }

        std::optional<ProjectAudioSourceSnapshot> getAudioSource(ProjectObjectId audioSourceId) const override {
            auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> std::optional<ProjectAudioSourceSnapshot> {
                if (!track)
                    return std::nullopt;
                for (const auto& clip : track->clipManager().getAllClips()) {
                    if (clip.clipType != ClipType::Audio)
                        continue;
                    if (audioSourceObjectId(*track, clip) != audioSourceId)
                        continue;
                    ProjectAudioSourceSnapshot snapshot;
                    snapshot.audioSourceId = audioSourceId;
                    snapshot.clipId = clipObjectId(*track, &clip, clip.clipId);
                    snapshot.filepath = clip.filepath;
                    snapshot.sourceNodeId = clip.sourceNodeInstanceId;
                    if (auto info = audio_source_repository_->getAudioSourceInfo(audioSourceId, clip.filepath)) {
                        snapshot.channelCount = info->channelCount;
                        snapshot.sampleRate = info->sampleRate;
                        snapshot.frameCount = info->frameCount;
                    }
                    return snapshot;
                }
                return std::nullopt;
            };

            if (auto found = findOnTrack(master_timeline_track_))
                return found;
            for (const auto& track : timeline_tracks_)
                if (auto found = findOnTrack(track))
                    return found;
            return std::nullopt;
        }

        bool readClipUmpContent(
            ProjectObjectId clipId,
            std::vector<uapmd_ump_t>& events,
            std::vector<uint64_t>& timestampsInTicks,
            uint32_t& tickResolution) const override {
            events.clear();
            timestampsInTicks.clear();
            tickResolution = 0;

            auto found = findClipById(clipId);
            if (!found || found->second.clipType != ClipType::Midi)
                return false;

            auto sourceNode = found->first->getSourceNode(found->second.sourceNodeInstanceId);
            auto* midiSource = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
            if (!midiSource)
                return false;

            events = midiSource->umpEvents();
            timestampsInTicks = midiSource->eventTimestampsTicks();
            tickResolution = midiSource->tickResolution();
            return true;
        }

        bool readAudioSourceSamples(
            ProjectObjectId audioSourceId,
            int64_t startFrame,
            int64_t frameCount,
            float** destination,
            uint32_t destinationChannels) const override {
            auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) {
                if (!track)
                    return false;
                for (const auto& clip : track->clipManager().getAllClips()) {
                    if (clip.clipType != ClipType::Audio)
                        continue;
                    if (audioSourceObjectId(*track, clip) != audioSourceId)
                        continue;
                    return audio_source_repository_->readAudioSourceSamples(
                        audioSourceId,
                        clip.filepath,
                        startFrame,
                        frameCount,
                        destination,
                        destinationChannels);
                }
                return false;
            };

            if (findOnTrack(master_timeline_track_))
                return true;
            for (const auto& track : timeline_tracks_)
                if (findOnTrack(track))
                    return true;
            return false;
        }

        bool replaceTrackGraphType(
            int32_t trackIndex,
            const std::string& graphTypeId,
            size_t eventBufferSizeInBytes) override {
            auto* provider = audio_graph_provider_registry_.get(graphTypeId);
            if (!provider)
                return false;

            SequencerTrack* track = trackIndex == kMasterTrackIndex
                ? engine_.masterTrack()
                : (trackIndex >= 0 && trackIndex < static_cast<int32_t>(engine_.tracks().size())
                    ? engine_.tracks()[static_cast<size_t>(trackIndex)]
                    : nullptr);
            if (!track)
                return false;

            if (track->graph().providerId() == provider->id())
                return true;

            auto newGraph = provider->createGraph(eventBufferSizeInBytes);
            return engine_.replaceTrackGraph(trackIndex, std::move(newGraph));
        }

        bool materializeProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            size_t eventBufferSizeInBytes) override {
            if (!projectTrack || !sequencerTrack || !projectTrack->graph())
                return true;

            auto* provider = audio_graph_provider_registry_.get(projectTrack->graph()->graphType());
            if (!provider)
                return false;

            int32_t trackIndex = -1;
            if (engine_.masterTrack() == sequencerTrack)
                trackIndex = kMasterTrackIndex;
            else {
                auto tracks = engine_.tracks();
                for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
                    if (tracks[static_cast<size_t>(i)] == sequencerTrack) {
                        trackIndex = i;
                        break;
                    }
                }
            }

            if (trackIndex == -1)
                return false;
            if (!replaceTrackGraphType(trackIndex, provider->id(), eventBufferSizeInBytes))
                return false;
            return provider->deserializeRuntimeGraph(
                projectTrack->graph(), sequencerTrack->graph(), sequencerTrack->orderedInstanceIds());
        }

        bool saveProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            const std::filesystem::path& projectDir,
            const std::filesystem::path& graphDir,
            const std::string& scopeLabel,
            std::string& error) override {
            if (!projectTrack || !sequencerTrack)
                return true;

            auto* provider = audio_graph_provider_registry_.get(sequencerTrack->graph());
            if (!provider)
                return false;

            auto graphFilename = std::format(
                "{}.graph.json",
                urlEscapeFilenameComponent(scopeLabel));
            auto graphPath = graphDir / graphFilename;
            auto recordedPath = graphPath;
            if (!projectDir.empty())
                recordedPath = makeRelativePath(projectDir, graphPath);

            std::vector<uint8_t> graphBytes;
            if (!provider->saveProjectGraph(projectTrack->graph(), graphBytes)) {
                error = std::format("Failed to serialize graph {}", graphPath.string());
                return false;
            }

            if (!writeBinaryFile(graphPath, graphBytes, error))
                return false;

            auto graph = UapmdProjectPluginGraphData::create();
            graph->graphType(provider->id());
            graph->externalFile(recordedPath);
            projectTrack->graph(std::move(graph));
            return true;
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
            bool needsFileSave) {
            ClipAddResult result;

            int32_t sourceNodeId = next_source_node_id_++;
            auto sourceNode = std::make_unique<MidiClipSourceNode>(
                sourceNodeId,
                std::move(umpEvents),
                std::move(umpTickTimestamps),
                tickResolution,
                clipTempo,
                static_cast<double>(sampleRate_),
                std::move(tempoChanges),
                std::move(timeSignatureChanges)
            );

            int64_t durationSamples = sourceNode->totalLength();

            ClipData clip;
            clip.clipType = ClipType::Midi;
            clip.position = position;
            clip.durationSamples = durationSamples;
            clip.sourceNodeInstanceId = sourceNodeId;
            clip.filepath = filepath;
            clip.needsFileSave = needsFileSave;
            clip.tickResolution = tickResolution;
            clip.clipTempo = clipTempo;
            clip.gain = 1.0;
            clip.muted = false;
            clip.name = clipName.empty() ? "MIDI Clip" : clipName;
            clip.setTimeReference(TimeReference::fromContainerStart({}, position.toSeconds(sampleRate_)), sampleRate_);
            clip.nrpnToParameterMapping = nrpnToParameterMapping;

            int32_t clipId = timelineTrack.addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
                applyAuthoritativeTempoMapToMusicalClips();
                emitClipAdded(timelineTrack, clipId, sourceNodeId);
                emitMasterTrackChanged("master-track-content-changed");
                notifyTimelineChanged();
            } else {
                result.error = "Failed to add MIDI clip to track";
            }
            return result;
        }

        ClipAddResult addAudioClipToTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps) {
            ClipAddResult result;
            if (!reader) {
                result.error = "Invalid audio file reader";
                return result;
            }

            int32_t sourceNodeId = next_source_node_id_++;
            auto sourceNode = std::make_unique<AudioFileSourceNode>(
                sourceNodeId,
                std::move(reader),
                static_cast<double>(sampleRate_),
                audioWarps
            );

            int64_t durationSamples = sourceNode->totalLength();

            ClipData clip;
            clip.position = position;
            clip.durationSamples = durationSamples;
            clip.sourceNodeInstanceId = sourceNodeId;
            clip.gain = 1.0;
            clip.muted = false;
            clip.filepath = filepath;
            clip.setTimeReference(TimeReference::fromContainerStart({}, position.toSeconds(sampleRate_)), sampleRate_);
            clip.markers = std::move(markers);
            clip.audioWarps = std::move(audioWarps);

            int32_t clipId = timelineTrack.addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
                emitClipAdded(timelineTrack, clipId, sourceNodeId);
                notifyTimelineChanged();
            } else {
                result.error = "Failed to add clip to track";
            }
            return result;
        }

        ClipAddResult addAudioClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath) override
        {
            ClipAddResult result;
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
                result.error = "Invalid track index";
                return result;
            }
            return addAudioClipToTrack(
                *timeline_tracks_[static_cast<size_t>(trackIndex)],
                position,
                std::move(reader),
                filepath,
                {},
                {});
        }

        ClipAddResult addMidiClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            const std::string& filepath,
            bool nrpnToParameterMapping = false) override
        {
            ClipAddResult result;
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
                result.error = "Invalid track index";
                return result;
            }

            auto clipInfo = MidiClipReader::readAnyFormat(filepath);
            if (!clipInfo.success) {
                result.error = clipInfo.error;
                return result;
            }
            return addMidiClipToTimelineTrack(
                *timeline_tracks_[static_cast<size_t>(trackIndex)],
                position,
                filepath,
                std::move(clipInfo.ump_data),
                std::move(clipInfo.ump_tick_timestamps),
                clipInfo.tick_resolution,
                clipInfo.tempo,
                std::move(clipInfo.tempo_changes),
                std::move(clipInfo.time_signature_changes),
                std::filesystem::path(filepath).stem().string(),
                nrpnToParameterMapping,
                false);
        }

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
            bool needsFileSave) override
        {
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
                ClipAddResult result;
                result.error = "Invalid track index";
                return result;
            }
            return addMidiClipToTimelineTrack(
                *timeline_tracks_[static_cast<size_t>(trackIndex)],
                position,
                "",
                std::move(umpEvents),
                std::move(umpTickTimestamps),
                tickResolution,
                clipTempo,
                std::move(tempoChanges),
                std::move(timeSignatureChanges),
                clipName,
                nrpnToParameterMapping,
                needsFileSave);
        }

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
            const std::string& filepath) override
        {
            return addMidiClipToTimelineTrack(
                *master_timeline_track_,
                position,
                filepath,
                std::move(umpEvents),
                std::move(umpTickTimestamps),
                tickResolution,
                clipTempo,
                std::move(tempoChanges),
                std::move(timeSignatureChanges),
                clipName,
                false,
                needsFileSave);
        }

        bool removeClipFromTrack(int32_t trackIndex, int32_t clipId) override {
            bool removed = false;
            TimelineTrack* targetTrack = nullptr;
            std::optional<ClipData> removedClip{};
            if (trackIndex == kMasterTrackIndex) {
                targetTrack = master_timeline_track_.get();
            } else if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())) {
                targetTrack = timeline_tracks_[static_cast<size_t>(trackIndex)].get();
            }
            if (targetTrack) {
                if (auto* clip = targetTrack->clipManager().getClip(clipId))
                    removedClip = *clip;
                removed = targetTrack->removeClip(clipId);
            }
            if (removed) {
                applyAuthoritativeTempoMapToMusicalClips();
                if (targetTrack && removedClip)
                    emitClipRemoved(*targetTrack, *removedClip);
                if (removedClip && removedClip->clipType == ClipType::Midi)
                    emitMasterTrackChanged("master-track-content-changed");
                notifyTimelineChanged();
            }
            return removed;
        }

        ProjectResult loadProject(const std::filesystem::path& projectFile) override {
            suppress_timeline_notification_ = true;
            suppress_project_document_events_ = true;
            struct SuppressGuard {
                bool& timelineFlag;
                bool& projectDocumentEventsFlag;
                ~SuppressGuard() {
                    timelineFlag = false;
                    projectDocumentEventsFlag = false;
                }
            } suppressGuard{suppress_timeline_notification_, suppress_project_document_events_};
            ProjectResult result;
            if (projectFile.empty()) {
                result.error = "Project path is empty";
                return result;
            }

            auto project = UapmdProjectDataReader::read(projectFile);
            if (!project) {
                result.error = "Failed to parse project file";
                return result;
            }

            auto projectDir = projectFile.parent_path();

            timeline_.isPlaying = false;
            timeline_.playheadPosition = TimelinePosition{};
            timeline_.loopEnabled = false;
            next_timeline_track_reference_ = 1;
            master_timeline_track_ = std::make_shared<TimelineTrack>(
                std::string("master_track"),
                0,
                sampleRate_ > 0 ? static_cast<double>(sampleRate_) : 48000.0,
                bufferSizeInFrames_);

            // Clear all existing tracks via engine (which calls onTrackRemoved for each)
            // NOTE: SequencerEngine::tracks() returns a transient snapshot, so refresh it
            // every iteration to ensure we see the latest state.
            while (true) {
                auto& snapshot = engine_.tracks();
                if (snapshot.empty())
                    break;
                engine_.removeTrack(static_cast<uapmd_track_index_t>(snapshot.size() - 1));
            }

            std::atomic<int> pending_plugins{0};
            struct LoadedClipRef {
                TimelineTrack* track{nullptr};
                int32_t clipId{-1};
                std::string clipReferenceId;
            };
            std::unordered_map<UapmdProjectClipData*, LoadedClipRef> loadedClipRefs;

            // FIXME: we might have to reconsider how we adapt plugin instances instantiated here to the graph.
            //  Currently, `UapmdPluginGraphBuilder::build()` is practically no-op, but the project loads.
            //  It is because it is already added in a linear manner.
            //  But that may not be the right thing depending on the graphs (such as, full DAG).
            auto loadPluginsForTrack = [this, &projectDir, &pending_plugins](UapmdProjectTrackData* projectTrack, int32_t trackIndex) {
                if (!projectTrack)
                    return;
                auto* graphData = projectTrack->graph();
                if (!graphData)
                    return;
                auto* provider = audio_graph_provider_registry_.get(graphData->graphType());
                auto externalGraphFile = graphData->externalFile();
                if (!externalGraphFile.empty()) {
                    if (provider) {
                        auto resolvedGraphFile = makeAbsolutePath(projectDir, externalGraphFile);
                        std::vector<uint8_t> graphBytes;
                        std::ifstream graphInput(resolvedGraphFile, std::ios::binary);
                        if (graphInput)
                            graphBytes.assign(std::istreambuf_iterator<char>(graphInput), {});
                        auto loadedGraphData = graphBytes.empty()
                            ? std::unique_ptr<UapmdProjectPluginGraphData>{}
                            : loadSerializedProjectGraph(*provider, *graphData, graphBytes);
                        if (!loadedGraphData) {
                            std::cerr << "Warning: Failed to load external graph file "
                                      << resolvedGraphFile << ". Falling back to embedded graph data." << std::endl;
                        } else {
                            projectTrack->graph(std::move(loadedGraphData));
                            graphData = projectTrack->graph();
                        }
                    }
                }
                if (!provider)
                    return;
                auto* pluginHost = engine_.pluginHost();
                std::vector<remidy::PluginCatalogEntry> catalogEntries;
                bool catalogLoaded = false;
                auto ensureCatalogLoaded = [&]() -> std::vector<remidy::PluginCatalogEntry>& {
                    if (!catalogLoaded && pluginHost) {
                        catalogEntries = pluginHost->pluginCatalogEntries();
                        catalogLoaded = true;
                    }
                    return catalogEntries;
                };
                auto catalogHasPlugin = [&](const std::string& format, const std::string& pluginId) -> bool {
                    if (!pluginHost)
                        return true; // Cannot verify without host; assume valid
                    if (pluginId.empty())
                        return false;
                    auto& entries = ensureCatalogLoaded();
                    return std::any_of(entries.begin(), entries.end(),
                        [&](remidy::PluginCatalogEntry& entry) {
                            return entry.format() == format && entry.pluginId() == pluginId;
                        });
                };
                auto catalogFindByName = [&](const std::string& format, const std::string& displayName) -> std::string {
                    if (!pluginHost || displayName.empty())
                        return {};
                    auto& entries = ensureCatalogLoaded();
                    std::string resolvedId;
                    for (auto& entry : entries) {
                        if (entry.format() == format && entry.displayName() == displayName) {
                            if (resolvedId.empty())
                                resolvedId = entry.pluginId();
                            else if (resolvedId != entry.pluginId())
                                return {}; // Ambiguous
                        }
                    }
                    return resolvedId;
                };

                for (const auto& plugin : provider->getPluginNodeDataListFrom(graphData)) {
                    if (plugin.format.empty()) {
                        std::cerr << "Warning: Skipping plugin node with missing format while loading project." << std::endl;
                        continue;
                    }
                    std::string format = plugin.format;
                    std::string pluginId = plugin.plugin_id;
                    std::string stateFile = plugin.state_file;
                    const std::string pluginName = plugin.display_name;
                    const int32_t groupIndex = plugin.group_index;

                    if (pluginId.empty()) {
                        auto fallbackId = catalogFindByName(format, pluginName);
                        if (!fallbackId.empty()) {
                            std::cerr << "Info: Plugin \"" << pluginName
                                      << "\" missing ID; resolved using catalog entry ID " << fallbackId << "." << std::endl;
                            pluginId = fallbackId;
                        }
                    } else if (!catalogHasPlugin(format, pluginId)) {
                        auto fallbackId = catalogFindByName(format, pluginName);
                        if (!fallbackId.empty()) {
                            std::cerr << "Info: Plugin \"" << (pluginName.empty() ? pluginId : pluginName)
                                      << "\" not found by ID; substituting catalog entry ID " << fallbackId << "." << std::endl;
                            pluginId = fallbackId;
                        }
                    }

                    if (pluginId.empty()) {
                        std::cerr << "Warning: Unable to resolve plugin entry (format=" << format
                                  << ", name=" << pluginName << "). Plugin will be skipped." << std::endl;
                        continue;
                    }

                    std::filesystem::path resolvedState;
                    if (!stateFile.empty())
                        resolvedState = makeAbsolutePath(projectDir, stateFile);

                    const std::string pluginLabel = pluginName.empty() ? pluginId : pluginName;

                    pending_plugins.fetch_add(1, std::memory_order_relaxed);
                    engine_.addPluginToTrack(trackIndex, format, pluginId,
                        [this, resolvedState, groupIndex, &pending_plugins, pluginLabel, pluginId, format](int32_t instanceId, int32_t, std::string error) {
                            if (!error.empty()) {
                                std::cerr << "Warning: Failed to instantiate plugin " << pluginLabel
                                          << " (" << format << ", ID=" << pluginId << "): " << error << std::endl;
                            } else if (instanceId >= 0) {
                                if (!resolvedState.empty()) {
                                    auto* instance = engine_.getPluginInstance(instanceId);
                                    if (instance) {
                                        std::ifstream f(resolvedState, std::ios::binary);
                                        if (f) {
                                            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
                                            instance->loadStateSync(data);
                                        }
                                    }
                                }
                                // Restore saved group assignment (overrides auto-assigned group)
                                if (groupIndex >= 0 && groupIndex <= 15)
                                    engine_.setInstanceGroup(instanceId, static_cast<uint8_t>(groupIndex));
                            }
                            pending_plugins.fetch_sub(1, std::memory_order_release);
                        });
                }
            };

            auto* masterProjectTrack = project->masterTrack();
            const bool hasExplicitMasterTrackClips = masterProjectTrack && !masterProjectTrack->clips().empty();

            auto& tracks = project->tracks();
            for (size_t i = 0; i < tracks.size(); ++i) {
                int32_t trackIndex = engine_.addEmptyTrack();
                if (trackIndex < 0) {
                    result.error = "Failed to create track";
                    return result;
                }

                loadPluginsForTrack(tracks[i], trackIndex);

                for (auto& clip : tracks[i]->clips()) {
                    if (!clip)
                        continue;

                    auto absoluteSamples = static_cast<int64_t>(clip->absolutePositionInSamples());
                    TimelinePosition position;
                    position.samples = absoluteSamples;

                    const auto clipFile = clip->file();
                    const auto clipType = clip->clipType();
                    std::filesystem::path resolvedPath = clipFile;
                    if (!resolvedPath.empty())
                        resolvedPath = makeAbsolutePath(projectDir, resolvedPath);

                    if (clipType == "midi") {
                        if (resolvedPath.empty()) {
                            result.error = "MIDI clip is missing file path";
                            return result;
                        }
                        auto clipInfo = MidiClipReader::readAnyFormat(resolvedPath);
                        if (!clipInfo.success) {
                            result.error = clipInfo.error.empty() ? "Failed to parse MIDI clip" : clipInfo.error;
                            return result;
                        }
                        auto separated = MidiClipReader::separateMasterTrackEvents(std::move(clipInfo));
                        auto& musicalClip = separated.musicalClip;
                        auto& masterClip = separated.masterTrackClip;
                        if (separated.hasMusicalClip()) {
                            double clipTempo = musicalClip.tempo_changes.empty() ? 120.0 : musicalClip.tempo_changes.front().bpm;
                            if (clipTempo <= 0.0) clipTempo = 120.0;
                            auto loadResult = addMidiClipToTrack(
                                trackIndex, position,
                                std::move(musicalClip.ump_data),
                                std::move(musicalClip.ump_tick_timestamps),
                                musicalClip.tick_resolution,
                                clipTempo,
                                std::move(musicalClip.tempo_changes),
                                std::move(musicalClip.time_signature_changes),
                                resolvedPath.filename().string(),
                                clip->nrpnToParameterMapping(),
                                separated.hasMasterTrackClip());
                            if (!loadResult.success) {
                                result.error = loadResult.error.empty() ? "Failed to load MIDI clip" : loadResult.error;
                                return result;
                            }
                            if (auto* loadedClip = timeline_tracks_[static_cast<size_t>(trackIndex)]->clipManager().getClip(loadResult.clipId)) {
                                loadedClip->markers = clip->markers();
                                loadedClip->audioWarps = clip->audioWarps();
                            }
                            auto* loadedClip = timeline_tracks_[static_cast<size_t>(trackIndex)]->clipManager().getClip(loadResult.clipId);
                            loadedClipRefs[clip.get()] = LoadedClipRef{
                                timeline_tracks_[static_cast<size_t>(trackIndex)].get(),
                                loadResult.clipId,
                                loadedClip ? loadedClip->referenceId : std::string{}};
                        }
                        if (!hasExplicitMasterTrackClips && separated.hasMasterTrackClip()) {
                            auto masterLoadResult = addMasterMidiClip(
                                position,
                                {},
                                {},
                                masterClip.tick_resolution,
                                masterClip.tempo,
                                std::move(masterClip.tempo_changes),
                                std::move(masterClip.time_signature_changes),
                                std::format("{} Meta", resolvedPath.filename().string()),
                                false,
                                "");
                            if (!masterLoadResult.success) {
                                result.error = masterLoadResult.error.empty() ? "Failed to load master track clip" : masterLoadResult.error;
                                return result;
                            }
                        }
                    } else {
                        auto reader = createAudioFileReaderFromPath(resolvedPath.string());
                        if (!reader) {
                            result.error = std::format("Failed to open audio clip {}", resolvedPath.string());
                            return result;
                        }
                        auto loadResult = addAudioClipToTrack(
                            *timeline_tracks_[static_cast<size_t>(trackIndex)],
                            position,
                            std::move(reader),
                            resolvedPath.string(),
                            clip->markers(),
                            clip->audioWarps());
                        if (!loadResult.success) {
                            result.error = loadResult.error.empty() ? "Failed to load audio clip" : loadResult.error;
                            return result;
                        }
                        auto* loadedClip = timeline_tracks_[static_cast<size_t>(trackIndex)]->clipManager().getClip(loadResult.clipId);
                        loadedClipRefs[clip.get()] = LoadedClipRef{
                            timeline_tracks_[static_cast<size_t>(trackIndex)].get(),
                            loadResult.clipId,
                            loadedClip ? loadedClip->referenceId : std::string{}};
                    }
                }
            }

            // Load master track clips (tempo/time-signature map)
            if (masterProjectTrack) {
                loadPluginsForTrack(masterProjectTrack, kMasterTrackIndex);
                for (auto& clip : masterProjectTrack->clips()) {
                    if (!clip || clip->clipType() != "midi")
                        continue;
                    auto resolvedPath = makeAbsolutePath(projectDir, clip->file());
                    if (resolvedPath.empty())
                        continue;
                    auto clipInfo = MidiClipReader::readAnyFormat(resolvedPath);
                    if (!clipInfo.success)
                        continue;
                    double clipTempo = clipInfo.tempo_changes.empty() ? 120.0 : clipInfo.tempo_changes.front().bpm;
                    if (clipTempo <= 0.0) clipTempo = 120.0;
                    if (!clipInfo.tempo_changes.empty())
                        timeline_.tempo = clipTempo;
                    TimelinePosition pos;
                    pos.samples = static_cast<int64_t>(clip->absolutePositionInSamples());
                    auto masterLoadResult = addMasterMidiClip(
                        pos,
                        std::move(clipInfo.ump_data),
                        std::move(clipInfo.ump_tick_timestamps),
                        clipInfo.tick_resolution,
                        clipTempo,
                        std::move(clipInfo.tempo_changes),
                        std::move(clipInfo.time_signature_changes),
                        resolvedPath.filename().string(),
                        false,
                        resolvedPath.string());
                    if (!masterLoadResult.success) {
                        result.error = masterLoadResult.error.empty() ? "Failed to load master track clip" : masterLoadResult.error;
                        return result;
                    }
                    auto* loadedClip = master_timeline_track_->clipManager().getClip(masterLoadResult.clipId);
                    if (loadedClip) {
                        loadedClip->markers = clip->markers();
                        loadedClip->audioWarps = clip->audioWarps();
                    }
                    loadedClipRefs[clip.get()] = LoadedClipRef{
                        master_timeline_track_.get(),
                        masterLoadResult.clipId,
                        loadedClip ? loadedClip->referenceId : std::string{}};
                }
            }

            auto applyAnchorToLoadedClip = [this, &loadedClipRefs](UapmdProjectClipData* projectClip) {
                if (!projectClip)
                    return;
                auto loadedIt = loadedClipRefs.find(projectClip);
                if (loadedIt == loadedClipRefs.end())
                    return;

                auto pos = projectClip->position();
                auto* targetTrack = loadedIt->second.track;
                if (!targetTrack)
                    return;

                TimeReference anchor = TimeReference::fromContainerStart();
                if (auto* anchorClip = dynamic_cast<UapmdProjectClipData*>(pos.anchor)) {
                    auto anchorIt = loadedClipRefs.find(anchorClip);
                    if (anchorIt != loadedClipRefs.end())
                        anchor.referenceId = anchorIt->second.clipReferenceId;
                }
                anchor.type = pos.origin == UapmdAnchorOrigin::End
                    ? TimeReferenceType::ContainerEnd
                    : TimeReferenceType::ContainerStart;
                anchor.offset = TimelinePosition(static_cast<int64_t>(pos.samples)).toSeconds(sampleRate_);
                targetTrack->clipManager().setClipAnchor(
                    loadedIt->second.clipId,
                    anchor,
                    sampleRate_);
                targetTrack->clipManager().setClipPosition(
                    loadedIt->second.clipId,
                    TimelinePosition(static_cast<int64_t>(projectClip->absolutePositionInSamples())));
            };

            for (auto* projectTrack : tracks)
                for (auto& clip : projectTrack->clips())
                    applyAnchorToLoadedClip(clip.get());
            if (masterProjectTrack)
                for (auto& clip : masterProjectTrack->clips())
                    applyAnchorToLoadedClip(clip.get());

            // Wait for all async plugin instantiations to complete
            while (pending_plugins.load(std::memory_order_acquire) > 0) {
                remidy::EventLoop::processQueuedTasks();
                std::this_thread::yield();
            }

            auto applyGraphConnections = [this](UapmdProjectTrackData* projectTrack, SequencerTrack* sequencerTrack) {
                if (!projectTrack || !sequencerTrack || !projectTrack->graph())
                    return;
                materializeProjectGraph(projectTrack, sequencerTrack, engine_.umpBufferSizeInBytes());
            };

            for (size_t i = 0; i < tracks.size() && i < engine_.tracks().size(); ++i)
                applyGraphConnections(tracks[i], engine_.tracks()[i]);
            if (masterProjectTrack)
                applyGraphConnections(masterProjectTrack, engine_.masterTrack());

            for (size_t i = 0; i < tracks.size() && i < engine_.tracks().size(); ++i) {
                if (tracks[i] && engine_.tracks()[i])
                    engine_.tracks()[i]->trackGain(tracks[i]->volume());
            }
            if (masterProjectTrack && engine_.masterTrack())
                engine_.masterTrack()->trackGain(masterProjectTrack->volume());

            suppress_timeline_notification_ = false;
            suppress_project_document_events_ = false;
            result.success = true;
            ProjectDocumentEvent loadedEvent(ProjectDocumentEventKind::ProjectLoaded, "project-loaded");
            loadedEvent.setProjectId(projectFile.string())
                .setFullResyncRecommended(true)
                .setDetail("source.file", projectFile.string());
            emitProjectDocumentEvent(std::move(loadedEvent));
            emitMasterTrackChanged("master-track-content-changed");
            notifyTimelineChanged();
            return result;
        }

        MasterTrackSnapshot buildMasterTrackSnapshot() override {
            MasterTrackSnapshot snapshot;
            const double sr = std::max(1.0, static_cast<double>(sampleRate_));
            auto appendTrackMeta = [&snapshot, sr](const std::shared_ptr<TimelineTrack>& track) {
                if (!track)
                    return;
                auto clips = track->clipManager().getAllClips();
                std::sort(clips.begin(), clips.end(), [](const ClipData& a, const ClipData& b) {
                    return a.clipId < b.clipId;
                });

                for (const auto& clip : clips) {
                    if (clip.clipType != ClipType::Midi)
                        continue;
                    auto sourceNode = track->getSourceNode(clip.sourceNodeInstanceId);
                    auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                    if (!midiNode)
                        continue;
                    appendMidiNodeMetaToSnapshot(snapshot, clip, *midiNode, sr);
                }
            };

            appendTrackMeta(master_timeline_track_);
            if (snapshot.empty()) {
                auto authoritative = findAuthoritativeTimelineMetaSource(false);
                if (authoritative && authoritative->node)
                    appendMidiNodeMetaToSnapshot(snapshot, authoritative->clip, *authoritative->node, sr);
            }

            std::stable_sort(snapshot.tempoPoints.begin(), snapshot.tempoPoints.end(),
                [](const MasterTrackSnapshot::TempoPoint& a, const MasterTrackSnapshot::TempoPoint& b) {
                    return a.timeSeconds < b.timeSeconds;
                });
            std::stable_sort(snapshot.timeSignaturePoints.begin(), snapshot.timeSignaturePoints.end(),
                [](const MasterTrackSnapshot::TimeSignaturePoint& a, const MasterTrackSnapshot::TimeSignaturePoint& b) {
                    return a.timeSeconds < b.timeSeconds;
                });

            return snapshot;
        }

        ContentBounds calculateContentBounds() const override {
            ContentBounds bounds;
            const double sr = std::max(1.0, static_cast<double>(sampleRate_));
            bool initialized = false;

            for (const auto& trackPtr : timeline_tracks_) {
                if (!trackPtr)
                    continue;

                auto clips = trackPtr->clipManager().getAllClips();
                if (clips.empty())
                    continue;

                for (const auto& clip : clips) {
                    auto absolute = clip.position;
                    const int64_t startSample = absolute.samples;
                    const int64_t endSample = startSample + std::max<int64_t>(0, clip.durationSamples);

                    if (!initialized || startSample < bounds.firstSample) {
                        bounds.firstSample = startSample;
                        bounds.firstSeconds = static_cast<double>(startSample) / sr;
                    }
                    if (!initialized || endSample > bounds.lastSample) {
                        bounds.lastSample = endSample;
                        bounds.lastSeconds = static_cast<double>(endSample) / sr;
                    }
                    initialized = true;
                }
            }

            bounds.hasContent = initialized;
            if (!initialized) {
                bounds.firstSample = 0;
                bounds.lastSample = 0;
                bounds.firstSeconds = 0.0;
                bounds.lastSeconds = 0.0;
            }
            return bounds;
        }

        void setTimelineChangedCallback(std::function<void()> cb) override {
            timeline_changed_callback_ = std::move(cb);
        }

        std::optional<std::vector<MidiNotePreview>> getMidiClipNotes(int32_t trackIndex, int32_t clipId) const override {
            TimelineTrack* track = nullptr;
            if (trackIndex == kMasterTrackIndex) {
                track = master_timeline_track_.get();
            } else if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())) {
                track = timeline_tracks_[static_cast<size_t>(trackIndex)].get();
            }
            if (!track) return std::nullopt;

            const ClipData* clip = track->clipManager().getClip(clipId);
            if (!clip || clip->clipType != ClipType::Midi) return std::nullopt;

            auto sourceNode = const_cast<TimelineTrack*>(track)->getSourceNode(clip->sourceNodeInstanceId);
            auto* midiSource = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
            if (!midiSource) return std::nullopt;

            const auto& events    = midiSource->umpEvents();
            const auto& timestamps = midiSource->eventTimestampsSamples();
            const double safeSR   = std::max(1.0, static_cast<double>(sampleRate_));
            const double clipDur  = std::max(0.01, static_cast<double>(midiSource->totalLength()) / safeSR);
            const double kMinDur  = 1.0 / 32.0;

            std::vector<MidiNotePreview> notes;
            std::unordered_map<uint32_t, size_t> activeNoteIndices;
            activeNoteIndices.reserve(64);

            const size_t eventCount = std::min(events.size(), timestamps.size());
            size_t i = 0;
            while (i < eventCount) {
                umppi::Ump ump1(events[i]);
                const int wordCount = ump1.getSizeInInts();
                const size_t safeCount = std::min(static_cast<size_t>(wordCount), eventCount - i);
                umppi::Ump ump = (safeCount >= 2) ? umppi::Ump(events[i], events[i + 1]) : ump1;
                const double eventSeconds = static_cast<double>(timestamps[i]) / safeSR;
                const auto msgType = ump.getMessageType();

                if (msgType == umppi::MessageType::MIDI1) {
                    const uint8_t status   = ump.getStatusCode();
                    const uint8_t channel  = ump.getChannelInGroup();
                    const uint8_t group    = ump.getGroup();
                    if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                        const uint8_t noteNum  = ump.getMidi1Note();
                        const uint8_t velocity = ump.getMidi1Velocity();
                        const uint32_t key     = (static_cast<uint32_t>(group) << 12) | (static_cast<uint32_t>(channel) << 7) | noteNum;
                        if (status == umppi::MidiChannelStatus::NOTE_ON && velocity > 0) {
                            MidiNotePreview n{};
                            n.startSeconds = eventSeconds;
                            n.note     = noteNum;
                            n.velocity = velocity / 127.0f;
                            activeNoteIndices[key] = notes.size();
                            notes.push_back(n);
                        } else {
                            auto it = activeNoteIndices.find(key);
                            if (it != activeNoteIndices.end()) {
                                notes[it->second].durationSeconds = std::max(kMinDur, eventSeconds - notes[it->second].startSeconds);
                                activeNoteIndices.erase(it);
                            }
                        }
                    }
                } else if (msgType == umppi::MessageType::MIDI2) {
                    const uint8_t  status  = ump.getStatusCode();
                    const uint8_t  channel = ump.getChannelInGroup();
                    const uint8_t  group   = ump.getGroup();
                    if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                        const uint8_t  noteNum = ump.getMidi2Note();
                        const uint16_t vel16   = ump.getMidi2Velocity16();
                        const uint32_t key     = (static_cast<uint32_t>(group) << 12) | (static_cast<uint32_t>(channel) << 7) | noteNum;
                        if (status == umppi::MidiChannelStatus::NOTE_ON && vel16 > 0) {
                            MidiNotePreview n{};
                            n.startSeconds = eventSeconds;
                            n.note     = noteNum;
                            n.velocity = vel16 / 65535.0f;
                            activeNoteIndices[key] = notes.size();
                            notes.push_back(n);
                        } else {
                            auto it = activeNoteIndices.find(key);
                            if (it != activeNoteIndices.end()) {
                                notes[it->second].durationSeconds = std::max(kMinDur, eventSeconds - notes[it->second].startSeconds);
                                activeNoteIndices.erase(it);
                            }
                        }
                    }
                }
                i += static_cast<size_t>(std::max(1, wordCount));
            }
            for (auto& [key, idx] : activeNoteIndices)
                notes[idx].durationSeconds = std::max(kMinDur, clipDur - notes[idx].startSeconds);

            return notes;
        }

        uint32_t maxTrackLatencyInSamples() override {
            uint32_t maxLatency = 0;
            auto& tracks = engine_.tracks();
            for (size_t i = 0; i < tracks.size(); ++i)
                maxLatency = std::max(maxLatency,
                                      engine_.trackRenderLeadInSamples(static_cast<int32_t>(i)));
            return maxLatency;
        }

        uint32_t trackRenderOffsetInSamples(int32_t trackIndex) override {
            if (trackIndex < 0)
                return 0;
            auto trackLatency = engine_.trackRenderLeadInSamples(trackIndex);
            auto masterLatency = engine_.masterTrackRenderLeadInSamples();
            return masterLatency + trackLatency;
        }

        uint32_t masterTrackRenderOffsetInSamples() override {
            return engine_.masterTrackRenderLeadInSamples();
        }

        bool trackHasLiveInput(int32_t trackIndex) override {
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= timeline_tracks_.size())
                return false;
            auto* track = timeline_tracks_[static_cast<size_t>(trackIndex)].get();
            return track ? track->hasDeviceInputSource() : false;
        }

        void processTracksAudio(AudioProcessContext& process, SequenceProcessContext& targetSequence) override {
            // Hold a snapshot reference for the duration of this callback so that
            // tracks added or removed on the UI thread cannot destroy TrackList
            // elements while we are iterating them.
            auto snapshot = std::atomic_load_explicit(
                &timeline_tracks_snapshot_, std::memory_order_acquire);

            auto wrapToLoopRange = [this](int64_t samplePosition) -> int64_t {
                if (!timeline_.loopEnabled || timeline_.loopEnd.samples <= timeline_.loopStart.samples)
                    return samplePosition;
                const auto loopLength = timeline_.loopEnd.samples - timeline_.loopStart.samples;
                if (samplePosition < timeline_.loopStart.samples)
                    return samplePosition;
                return timeline_.loopStart.samples +
                    ((samplePosition - timeline_.loopStart.samples) % loopLength);
            };

            const auto masterSnapshot = buildMasterTrackSnapshot();
            auto updateTransportMetaForPlayhead = [&masterSnapshot, this](TimelineState& state) {
                if (masterSnapshot.empty())
                    return;
                const double playheadSeconds =
                    static_cast<double>(state.playheadPosition.samples) /
                    std::max(1.0, static_cast<double>(sampleRate_));
                for (const auto& point : masterSnapshot.tempoPoints) {
                    if (point.timeSeconds <= playheadSeconds) {
                        state.tempo = point.bpm;
                    } else break;
                }
                for (const auto& point : masterSnapshot.timeSignaturePoints) {
                    if (point.timeSeconds <= playheadSeconds) {
                        state.timeSignatureNumerator = point.signature.numerator;
                        state.timeSignatureDenominator = point.signature.denominator;
                    } else break;
                }
            };

            timeline_.isPlaying = engine_.isPlaybackActive();
            const auto audiblePlayheadSamples = engine_.playbackPosition();
            const auto renderPlayheadRaw = engine_.renderPlaybackPosition();
            timeline_.playheadPosition.samples = wrapToLoopRange(audiblePlayheadSamples);
            updateTransportMetaForPlayhead(timeline_);

            // Update legacy_beats
            double secondsPerBeat = 60.0 / timeline_.tempo;
            int64_t samplesPerBeat = static_cast<int64_t>(secondsPerBeat * sampleRate_);
            if (samplesPerBeat > 0) {
                timeline_.playheadPosition.legacy_beats =
                    static_cast<double>(timeline_.playheadPosition.samples) / static_cast<double>(samplesPerBeat);
            }

            // Sync to MasterContext
            TimelineState renderTransport = timeline_;
            renderTransport.playheadPosition.samples = wrapToLoopRange(
                (timeline_.isPlaying || renderPlayheadRaw != audiblePlayheadSamples) ?
                    renderPlayheadRaw : audiblePlayheadSamples
            );
            updateTransportMetaForPlayhead(renderTransport);
            const double renderSecondsPerBeat = 60.0 / renderTransport.tempo;
            const int64_t renderSamplesPerBeat = static_cast<int64_t>(renderSecondsPerBeat * sampleRate_);
            if (renderSamplesPerBeat > 0) {
                renderTransport.playheadPosition.legacy_beats =
                    static_cast<double>(renderTransport.playheadPosition.samples) /
                    static_cast<double>(renderSamplesPerBeat);
            }

            auto& masterCtx = process.masterContext();
            masterCtx.playbackPositionSamples(renderTransport.playheadPosition.samples);
            masterCtx.isPlaying(timeline_.isPlaying);
            uint32_t tempoMicros = static_cast<uint32_t>(60000000.0 / renderTransport.tempo);
            masterCtx.tempo(tempoMicros);
            masterCtx.timeSignatureNumerator(renderTransport.timeSignatureNumerator);
            masterCtx.timeSignatureDenominator(renderTransport.timeSignatureDenominator);

            // Process each timeline track into the target sequencer context.
            // targetSequence.tracks[i] points to a pump ring-buffer slot when called
            // from pumpAudio(), or to engine_.data().tracks[i] on the legacy path.
            if (!snapshot) return;
            for (size_t i = 0; i < snapshot->size() && i < targetSequence.tracks.size(); ++i) {
                auto* trackContext = targetSequence.tracks[i];
                if (!trackContext)
                    continue;

                // Clamp against the track's buffer capacity to prevent overruns
                const auto safeFrames = static_cast<int32_t>(std::min(
                    static_cast<size_t>(process.frameCount()),
                    trackContext->audioBufferCapacityInFrames()));

                // Copy device input channels
                if (process.audioInBusCount() > 0 && trackContext->audioInBusCount() > 0) {
                    const uint32_t deviceChannels = std::min(
                        static_cast<uint32_t>(process.inputChannelCount(0)),
                        static_cast<uint32_t>(trackContext->inputChannelCount(0))
                    );
                    for (uint32_t ch = 0; ch < deviceChannels; ++ch) {
                        const float* src = process.getFloatInBuffer(0, ch);
                        float* dst = trackContext->getFloatInBuffer(0, ch);
                        if (src && dst)
                            std::memcpy(dst, src, safeFrames * sizeof(float));
                    }
                }

                auto renderTimeline = renderTransport;
                TimelinePosition renderPosition{};
                const auto trackOffset = trackRenderOffsetInSamples(static_cast<int32_t>(i));
                const auto renderBaseSample =
                    (timeline_.isPlaying || renderPlayheadRaw != audiblePlayheadSamples) ?
                        renderPlayheadRaw :
                        audiblePlayheadSamples;
                int64_t renderStartSample =
                    renderBaseSample + static_cast<int64_t>(trackOffset);
                if (renderStartSample < 0)
                    renderStartSample = 0;
                renderStartSample = wrapToLoopRange(renderStartSample);
                renderPosition.samples = renderStartSample;
                renderTimeline.seekTo(renderPosition, sampleRate_);
                updateTransportMetaForPlayhead(renderTimeline);

                int32_t destinationOffsetFrames = 0;
                int32_t remainingFrames = safeFrames;
                int64_t segmentStartSample = renderStartSample;
                while (remainingFrames > 0) {
                    auto segmentTimeline = renderTransport;
                    TimelinePosition segmentPosition{};
                    segmentPosition.samples = wrapToLoopRange(segmentStartSample);
                    segmentTimeline.seekTo(segmentPosition, sampleRate_);
                    updateTransportMetaForPlayhead(segmentTimeline);

                    int32_t segmentFrames = remainingFrames;
                    bool wrapsAtLoopEnd = false;
                    if (timeline_.loopEnabled &&
                        timeline_.loopEnd.samples > timeline_.loopStart.samples &&
                        segmentStartSample < timeline_.loopEnd.samples &&
                        segmentStartSample + remainingFrames > timeline_.loopEnd.samples) {
                        segmentFrames = static_cast<int32_t>(timeline_.loopEnd.samples - segmentStartSample);
                        wrapsAtLoopEnd = true;
                    }

                    if (segmentFrames <= 0)
                        break;

                    (*snapshot)[i]->processAudioForRenderSegment(
                        *trackContext,
                        segmentTimeline,
                        segmentStartSample,
                        destinationOffsetFrames,
                        segmentFrames);

                    destinationOffsetFrames += segmentFrames;
                    remainingFrames -= segmentFrames;
                    segmentStartSample = wrapsAtLoopEnd
                        ? timeline_.loopStart.samples
                        : (segmentStartSample + segmentFrames);
                }
            }
        }

        void onTrackAdded(uint32_t outputChannels, double sampleRate, uint32_t bufferSizeInFrames) override {
            sampleRate_ = static_cast<int32_t>(sampleRate);
            bufferSizeInFrames_ = bufferSizeInFrames;
            master_timeline_track_->reconfigureBuffers(0, bufferSizeInFrames);

            const std::string trackReferenceId = std::format("track_{}", next_timeline_track_reference_++);
            auto newTrack = std::make_shared<TimelineTrack>(trackReferenceId, outputChannels, sampleRate, bufferSizeInFrames);

            newTrack->setNrpnParameterCallback(
                [this, trackReferenceId](uint8_t group, uint32_t paramIdx, uint32_t rawValue, bool isRelative) {
                    auto& seqTracks = engine_.tracks();
                    uapmd_track_index_t trackIndex = -1;
                    auto tracks = this->tracks();
                    for (size_t i = 0; i < tracks.size(); ++i) {
                        if (tracks[i] && tracks[i]->referenceId() == trackReferenceId) {
                            trackIndex = static_cast<uapmd_track_index_t>(i);
                            break;
                        }
                    }
                    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= seqTracks.size())
                        return;
                    auto* seqTrack = seqTracks[static_cast<size_t>(trackIndex)];
                    if (!seqTrack)
                        return;
                    for (int32_t instanceId : seqTrack->orderedInstanceIds()) {
                        // Only target the instance whose UMP group matches the event.
                        if (seqTrack->getInstanceGroup(instanceId) != group)
                            continue;
                        double value;
                        if (isRelative) {
                            auto* inst = engine_.getPluginInstance(instanceId);
                            if (!inst)
                                continue;
                            value = inst->getParameterValue(static_cast<int32_t>(paramIdx))
                                    + static_cast<double>(static_cast<int32_t>(rawValue)) / INT32_MAX;
                        } else {
                            value = static_cast<double>(rawValue) / UINT32_MAX;
                        }
                        engine_.setParameterValue(instanceId, static_cast<int32_t>(paramIdx), value);
                    }
                });

            const auto trackIndex = static_cast<int32_t>(timeline_tracks_.size());
            const auto eventTrackId = newTrack->referenceId();
            timeline_tracks_.emplace_back(std::move(newTrack));
            rebuildTrackSnapshot();

            ProjectDocumentEvent event(ProjectDocumentEventKind::TrackAdded, "track-added");
            event.setTrackId(eventTrackId)
                .setTrackIndex(trackIndex);
            emitProjectDocumentEvent(std::move(event));
        }

        void onTrackRemoved(size_t trackIndex) override {
            if (trackIndex < timeline_tracks_.size()) {
                const auto eventTrackId = timeline_tracks_[trackIndex]->referenceId();
                timeline_tracks_.erase(timeline_tracks_.begin() + static_cast<long>(trackIndex));
                rebuildTrackSnapshot();

                ProjectDocumentEvent event(ProjectDocumentEventKind::TrackRemoved, "track-removed");
                event.setTrackId(eventTrackId)
                    .setTrackIndex(static_cast<int32_t>(trackIndex));
                emitProjectDocumentEvent(std::move(event));
            }
        }

        void onTrackGraphChanged(int32_t trackIndex) override {
            ProjectDocumentEvent event(ProjectDocumentEventKind::PluginGraphChanged, "plugin-graph-changed");
            event.setTrackIndex(trackIndex);
            if (trackIndex == kMasterTrackIndex) {
                if (master_timeline_track_)
                    event.setTrackId(master_timeline_track_->referenceId());
            } else if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())) {
                if (auto& track = timeline_tracks_[static_cast<size_t>(trackIndex)])
                    event.setTrackId(track->referenceId());
            }
            emitProjectDocumentEvent(std::move(event));
        }

    private:
        static std::filesystem::path makeRelativePath(
            const std::filesystem::path& baseDir,
            const std::filesystem::path& target)
        {
            if (baseDir.empty() || target.empty())
                return target;

            std::error_code ec;
            auto rel = std::filesystem::relative(target, baseDir, ec);
            if (ec)
                return target;

            for (const auto& part : rel) {
                if (part == "..")
                    return target;
            }
            return rel;
        }

        static std::filesystem::path makeAbsolutePath(
            const std::filesystem::path& baseDir,
            const std::filesystem::path& target)
        {
            if (target.empty())
                return target;
            if (target.is_absolute() || baseDir.empty())
                return std::filesystem::absolute(target);
            return std::filesystem::absolute(baseDir / target);
        }

        static std::string urlEscapeFilenameComponent(std::string_view value) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string escaped;
            escaped.reserve(value.size() * 3);
            for (unsigned char ch : value) {
                if ((ch >= 'A' && ch <= 'Z') ||
                    (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                    escaped.push_back(static_cast<char>(ch));
                    continue;
                }
                escaped.push_back('%');
                escaped.push_back(kHex[(ch >> 4) & 0xF]);
                escaped.push_back(kHex[ch & 0xF]);
            }
            return escaped;
        }

        static bool writeBinaryFile(
            const std::filesystem::path& path,
            const std::vector<uint8_t>& bytes,
            std::string& error)
        {
            std::error_code createDirEc;
            std::filesystem::create_directories(path.parent_path(), createDirEc);
            if (createDirEc) {
                error = std::format(
                    "Failed to create directory for {}: {}",
                    path.string(),
                    createDirEc.message());
                return false;
            }

            std::ofstream out(path, std::ios::binary);
            if (!out) {
                error = std::format("Failed to open {} for writing", path.string());
                return false;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                error = std::format("Failed to write {}", path.string());
                return false;
            }
            return true;
        }
    };

    std::unique_ptr<TimelineFacade> TimelineFacade::create(SequencerEngine& engine) {
        return std::make_unique<TimelineFacadeImpl>(engine);
    }

} // namespace uapmd
