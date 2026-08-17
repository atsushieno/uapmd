#include "TimelineFacadeImpl.hpp"

// The read-only document view and the address book: turning persistent
// identities into live objects and back, and emitting document events.

namespace uapmd {
    ProjectRevision TimelineFacadeImpl::currentRevision() const {
        return project_document_events_.currentRevision();
    }

    std::optional<ProjectObjectId> TimelineFacadeImpl::masterTrackId() const {
        if (!master_timeline_track_)
            return std::nullopt;
        return master_timeline_track_->referenceId();
    }

    std::vector<ProjectObjectId> TimelineFacadeImpl::trackIds() const {
        std::vector<ProjectObjectId> result;
        result.reserve(timeline_tracks_.size());
        for (const auto& track : timeline_tracks_)
            if (track)
                result.push_back(track->referenceId());
        return result;
    }

    std::vector<ProjectObjectId> TimelineFacadeImpl::clipIds(ProjectObjectId trackId) const {
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

    std::vector<ProjectObjectId> TimelineFacadeImpl::audioSourceIds() const {
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

    std::optional<ProjectTrackSnapshot> TimelineFacadeImpl::getTrack(ProjectObjectId trackId) const {
        auto* track = findTrackById(trackId);
        if (!track)
            return std::nullopt;
        return ProjectTrackSnapshot{
            .trackId = track->referenceId(),
            .trackIndex = trackIndexFor(*track),
            .masterTrack = track == master_timeline_track_.get()
        };
    }

    std::optional<ProjectClipSnapshot> TimelineFacadeImpl::getClip(ProjectObjectId clipId) const {
        auto found = findClipById(clipId);
        if (!found)
            return std::nullopt;
        return makeClipSnapshot(*found->first, found->second);
    }

    std::optional<ProjectAudioSourceSnapshot> TimelineFacadeImpl::getAudioSource(ProjectObjectId audioSourceId) const {        auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> std::optional<ProjectAudioSourceSnapshot> {
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
                } else if (clip.filepath.empty()) {
                    snapshot.channelCount = std::max<uint32_t>(1, track->channelCount());
                    snapshot.sampleRate = static_cast<double>(sampleRate_);
                    snapshot.frameCount = std::max<int64_t>(1, clip.durationSamples);
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

    bool TimelineFacadeImpl::readClipUmpContent(
            ProjectObjectId clipId,
            std::vector<uapmd_ump_t>& events,
            std::vector<uint64_t>& timestampsInTicks,
            uint32_t& tickResolution) const {
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

    bool TimelineFacadeImpl::readAudioSourceSamples(
            ProjectObjectId audioSourceId,
            int64_t startFrame,
            int64_t frameCount,
            float** destination,
            uint32_t destinationChannels) const {        auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) {
            if (!track)
                return false;
            for (const auto& clip : track->clipManager().getAllClips()) {
                if (clip.clipType != ClipType::Audio)
                    continue;
                if (audioSourceObjectId(*track, clip) != audioSourceId)
                    continue;
                if (clip.filepath.empty()) {
                    if (!destination || startFrame < 0 || frameCount < 0)
                        return false;
                    for (uint32_t ch = 0; ch < destinationChannels; ++ch)
                        if (destination[ch])
                            std::memset(destination[ch], 0, static_cast<size_t>(frameCount) * sizeof(float));
                    return true;
                }
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

    ProjectClipSnapshot TimelineFacadeImpl::makeClipSnapshot(const TimelineTrack& track, const ClipData& clip) const {        return ProjectClipSnapshot{
        .clipId = clipObjectId(track, &clip, clip.clipId),
            .trackId = track.referenceId(),
            .trackIndex = trackIndexFor(track),
            .clipNumericId = clip.clipId,
            .sourceNodeId = clip.sourceNodeInstanceId,
            .clipType = clip.clipType,
            .name = clip.name,
            .filepath = clip.filepath,
            .position = clip.position,
            .sampleRate = static_cast<double>(sampleRate_),
            .durationSamples = clip.durationSamples,
            .tickResolution = clip.tickResolution,
            .clipTempo = clip.clipTempo,
            .markers = clip.markers,
            .audioWarps = clip.audioWarps
        };
    }

    void TimelineFacadeImpl::emitClipAdded(TimelineTrack& track, int32_t clipId, int32_t sourceNodeId) {
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

    void TimelineFacadeImpl::emitClipRemoved(TimelineTrack& track, const ClipData& clip) {
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

    void TimelineFacadeImpl::emitClipChanged(TimelineTrack& track, const ClipData& clip, std::string type) {
        ProjectDocumentEvent event(ProjectDocumentEventKind::ClipChanged, std::move(type));
        event.setTrackId(track.referenceId())
            .setClipId(clipObjectId(track, &clip, clip.clipId))
            .setTrackIndex(trackIndexFor(track))
            .setClipNumericId(clip.clipId)
            .setDetail("source-node-id", static_cast<int64_t>(clip.sourceNodeInstanceId))
            .setDetail("clip-type", std::string(clip.clipType == ClipType::Audio ? "audio" : "midi"));
        if (!clip.filepath.empty())
            event.setDetail("source.file", clip.filepath);
        emitProjectDocumentEvent(std::move(event));
    }

    void TimelineFacadeImpl::emitMasterTrackChanged(std::string type) {
        ProjectDocumentEvent event(ProjectDocumentEventKind::MasterTrackChanged, std::move(type));
        event.setTrackId(master_timeline_track_ ? master_timeline_track_->referenceId() : "master_track")
            .setTrackIndex(kMasterTrackIndex);
        emitProjectDocumentEvent(std::move(event));
    }

    void TimelineFacadeImpl::emitTrackChanged(
            std::string_view trackReferenceId,
            std::string changeType) {
                const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        if (trackIndex == kMasterTrackIndex) {
            emitMasterTrackChanged(std::move(changeType));
            return;
        }
        ProjectDocumentEvent event(
            ProjectDocumentEventKind::TrackChanged,
            std::move(changeType));
        event.setTrackId(std::string(trackReferenceId))
            .setTrackIndex(trackIndex);
        emitProjectDocumentEvent(std::move(event));
    }

    std::string TimelineFacadeImpl::clipObjectId(const TimelineTrack& track, const ClipData* clip, int32_t clipId) {
        if (clip && !clip->referenceId.empty())
            return clip->referenceId;
        return std::format("{}::clip_{:08x}", track.referenceId(), static_cast<uint32_t>(clipId));
    }

    std::string TimelineFacadeImpl::audioSourceObjectId(const TimelineTrack& track, const ClipData& clip) {
        if (!clip.filepath.empty())
            return "audio-source:" + clip.filepath;
        return "audio-source:" + clipObjectId(track, &clip, clip.clipId);
    }

    size_t TimelineFacadeImpl::audioSourceReferenceCount(const std::string& audioSourceId) const {        auto countOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> size_t {
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

    TimelineTrack* TimelineFacadeImpl::findTrackById(const ProjectObjectId& trackId) const {
        if (master_timeline_track_ && master_timeline_track_->referenceId() == trackId)
            return master_timeline_track_.get();
        for (const auto& track : timeline_tracks_)
            if (track && track->referenceId() == trackId)
                return track.get();
        return nullptr;
    }

    std::optional<std::pair<TimelineTrack*, ClipData>> TimelineFacadeImpl::findClipById(const ProjectObjectId& clipId) const {        auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> std::optional<std::pair<TimelineTrack*, ClipData>> {
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

    int32_t TimelineFacadeImpl::trackIndexFor(const TimelineTrack& track) const {
        if (&track == master_timeline_track_.get())
            return kMasterTrackIndex;
        for (int32_t i = 0; i < static_cast<int32_t>(timeline_tracks_.size()); ++i)
            if (timeline_tracks_[static_cast<size_t>(i)].get() == &track)
                return i;
        return -1;
    }

    TimelineTrack* TimelineFacadeImpl::timelineTrack(std::string_view trackReferenceId) {
        return resolveTrackByReferenceId(trackReferenceId);
    }

    SequencerTrack* TimelineFacadeImpl::sequencerTrack(std::string_view trackReferenceId) {
        return resolveSequencerTrackByReferenceId(trackReferenceId);
    }

    int32_t TimelineFacadeImpl::trackIndex(std::string_view trackReferenceId) const {
        return trackIndexForPersistentId(trackReferenceId);
    }

    int32_t TimelineFacadeImpl::clipId(const ClipAddress& address) const {
        const auto* track = const_cast<TimelineFacadeImpl*>(this)
            ->resolveTrackByReferenceId(address.trackReferenceId);
        return track ? clipIdForReferenceId(*track, address.clipReferenceId) : -1;
    }

    int32_t TimelineFacadeImpl::pluginInstanceId(const PluginAddress& address) {
        return resolvePluginInstanceId(address.trackReferenceId, address.nodeId);
    }

    std::optional<ProjectObjectId> TimelineFacadeImpl::trackReferenceId(int32_t index) const {
        const auto* track = resolveTrack(index);
        if (!track)
            return std::nullopt;
        return track->referenceId();
    }

    std::optional<ClipAddress> TimelineFacadeImpl::clipAddress(
            int32_t index,
            int32_t clipIdentifier) const {
                const auto* track = resolveTrack(index);
        const auto* clip = track
            ? track->clipManager().getClip(clipIdentifier)
            : nullptr;
        if (!clip)
            return std::nullopt;
        return ClipAddress{
            .trackReferenceId = track->referenceId(),
            .clipReferenceId = clip->referenceId
        };
    }

    std::optional<PluginAddress> TimelineFacadeImpl::pluginAddress(int32_t instanceId) {
        return pluginTargetForInstance(instanceId);
    }

    int32_t TimelineFacadeImpl::trackIndexForPersistentId(std::string_view trackReferenceId) const {
        if (master_timeline_track_ && master_timeline_track_->referenceId() == trackReferenceId)
            return kMasterTrackIndex;
        for (size_t index = 0; index < timeline_tracks_.size(); ++index)
            if (timeline_tracks_[index]
                && timeline_tracks_[index]->referenceId() == trackReferenceId)
                return static_cast<int32_t>(index);
        return -1;
    }

    TimelineTrack* TimelineFacadeImpl::resolveTrackByReferenceId(std::string_view trackReferenceId) {
        if (master_timeline_track_ && master_timeline_track_->referenceId() == trackReferenceId)
            return master_timeline_track_.get();
        for (const auto& track : timeline_tracks_)
            if (track && track->referenceId() == trackReferenceId)
                return track.get();
        return nullptr;
    }

    int32_t TimelineFacadeImpl::clipIdForReferenceId(
            const TimelineTrack& track,
            std::string_view clipReferenceId) {
                for (const auto& clip : track.clipManager().getAllClips())
            if (clip.referenceId == clipReferenceId)
                return clip.clipId;
        return -1;
    }

    SequencerTrack* TimelineFacadeImpl::resolveSequencerTrackByReferenceId(
            std::string_view trackReferenceId) {
                const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        if (trackIndex == kMasterTrackIndex)
            return engine_.masterTrack();
        auto& tracks = engine_.tracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
            return nullptr;
        return tracks[static_cast<size_t>(trackIndex)];
    }

} // namespace uapmd
