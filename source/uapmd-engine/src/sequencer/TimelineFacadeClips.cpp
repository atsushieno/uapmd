#include "TimelineFacadeImpl.hpp"

// Clip creation, removal, content replacement and the clip property commands.

namespace uapmd {
    TimelineFacade::ClipAddResult TimelineFacadeImpl::addMidiClipToTimelineTrack(
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
            int32_t requestedClipId) {
                ClipAddResult result;

        // Normalize incoming ticks to the single project-wide PPQ, established by whichever
        // MIDI clip is added first. This keeps every clip's ticks directly comparable, so no
        // clip-to-clip rescaling is needed anywhere else once ticks enter the system.
        uint32_t effectiveResolution = tickResolution == 0 ? 480 : tickResolution;
        if (timeline_.projectTickResolution == 0)
            timeline_.projectTickResolution = effectiveResolution;
        else if (effectiveResolution != timeline_.projectTickResolution)
            MidiClipReader::rescaleTicks(umpTickTimestamps, tempoChanges, timeSignatureChanges,
                effectiveResolution, timeline_.projectTickResolution);
        tickResolution = timeline_.projectTickResolution;

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
        clip.clipId = requestedClipId;
        clip.referenceId = takePendingClipReferenceId();
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
        } else {result.error = "Failed to add MIDI clip to track";
        }
        return result;
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::addAudioClipToTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps,
            int32_t requestedClipId) {
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
        clip.clipId = requestedClipId;
        clip.referenceId = takePendingClipReferenceId();
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
        } else {result.error = "Failed to add clip to track";
        }
        return result;
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::addAudioClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            ProjectMutationOrigin origin) {
                ClipAddResult result;
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
        result.error = "Invalid track index";
        return result;
    }
    return recordAddedClip(
        trackIndex,
        addAudioClipToTrack(
            *timeline_tracks_[static_cast<size_t>(trackIndex)],
            position,
            std::move(reader),
            filepath,
            {},
            {}),
        origin);
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::addMidiClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            const std::string& filepath,
            bool nrpnToParameterMapping,
            ProjectMutationOrigin origin) {
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

        auto separated = MidiClipReader::separateMasterTrackEvents(std::move(clipInfo));
        auto& musicalClip = separated.musicalClip;
        auto& track = *timeline_tracks_[static_cast<size_t>(trackIndex)];
        const bool recordsHistory = origin == ProjectMutationOrigin::User
            || origin == ProjectMutationOrigin::Remote;
        const bool ownsCompound = recordsHistory
            && separated.hasMasterTrackClip()
            && !undo_engine_.state().compoundOpen;
        if (ownsCompound) {
            auto opened = undo_engine_.beginCompound("Import MIDI file", origin);
            if (!opened.succeeded()) {
                result.error = std::move(opened.error);
                return result;
            }
        }
        result = addMidiClipToTimelineTrack(
            track,
            position,
            filepath,
            std::move(musicalClip.ump_data),
            std::move(musicalClip.ump_tick_timestamps),
            musicalClip.tick_resolution,
            musicalClip.tempo,
            std::move(musicalClip.tempo_changes),
            std::move(musicalClip.time_signature_changes),
            std::filesystem::path(filepath).stem().string(),
            nrpnToParameterMapping,
            separated.hasMasterTrackClip());

        result = recordAddedClip(trackIndex, std::move(result), origin);
        if (!result.success) {
            if (ownsCompound)
                undo_engine_.cancelCompound();
            return result;
        }

        if (result.success && separated.hasMasterTrackClip()) {
            auto& masterClip = separated.masterTrackClip;
            auto masterResult = addMidiClipToTimelineTrack(
                *master_timeline_track_,
                position,
                "",
                {},
                {},
                masterClip.tick_resolution,
                masterClip.tempo,
                std::move(masterClip.tempo_changes),
                std::move(masterClip.time_signature_changes),
                std::format("{} Meta", std::filesystem::path(filepath).stem().string()),
                false,
                false);
            if (masterResult.success) {
                if (const auto* regularClip = track.clipManager().getClip(result.clipId)) {
                    if (!master_timeline_track_->clipManager().setClipAnchor(
                        masterResult.clipId,
                        TimeReference::fromContainerStart(regularClip->referenceId, 0.0),
                        sampleRate_)) {
                        removeClipRaw(*master_timeline_track_, masterResult.clipId);
                        masterResult.success = false;
                        masterResult.error = "Could not anchor the imported master MIDI clip";
                    }
                }
            }
            masterResult = recordAddedClip(
                kMasterTrackIndex, std::move(masterResult), origin);
            if (!masterResult.success) {
                if (ownsCompound)
                    undo_engine_.cancelCompound();
                result.success = false;
                result.error = masterResult.error.empty()
                    ? "Could not add the imported master MIDI clip"
                    : std::move(masterResult.error);
                return result;
            }
        }
        if (ownsCompound)
            undo_engine_.endCompound();
        return result;
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::addMidiClipToTrack(
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
            ProjectMutationOrigin origin) {    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
        ClipAddResult result;
        result.error = "Invalid track index";
        return result;
    }
    return recordAddedClip(
        trackIndex,
        addMidiClipToTimelineTrack(
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
            needsFileSave),
        origin);
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::addMasterMidiClip(
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
            ProjectMutationOrigin origin) {
                return recordAddedClip(
            kMasterTrackIndex,
            addMidiClipToTimelineTrack(
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
                needsFileSave),
            origin);
    }

    bool TimelineFacadeImpl::removeClipFromTrack(
            int32_t trackIndex,
            int32_t clipId,
            ProjectMutationOrigin origin) {
                auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return false;
        if (origin != ProjectMutationOrigin::User && origin != ProjectMutationOrigin::Remote)
            return removeClipRaw(*targetTrack, clipId);

        // Capture before beginning any document transaction: ARA archives
        // may not be created while the document is being edited.
        auto fragment = captureClipFragment(trackIndex, clipId);
        if (!fragment)
            return false;
        return performCapturedClipRemoval(
            targetTrack->referenceId(), std::move(*fragment), origin);
    }

    bool TimelineFacadeImpl::clearClipsFromTrack(
            int32_t trackIndex,
            ProjectMutationOrigin origin) {
                auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return false;
        const auto clips = targetTrack->clipManager().getAllClips();
        if (clips.empty())
            return false;

        if (origin != ProjectMutationOrigin::User && origin != ProjectMutationOrigin::Remote) {
            ProjectDocumentTransaction transaction(project_document_events_);
            bool removedAny = false;
            for (const auto& clip : clips)
                removedAny |= removeClipRaw(*targetTrack, clip.clipId);
            return removedAny;
        }

        std::vector<ProjectClipFragment> fragments;
        fragments.reserve(clips.size());
        for (const auto& clip : clips) {
            auto fragment = captureClipFragment(trackIndex, clip.clipId);
            if (!fragment)
                return false;
            fragments.push_back(std::move(*fragment));
        }

        const bool ownsCompound = !undo_engine_.state().compoundOpen;
        if (ownsCompound) {
            auto opened = undo_engine_.beginCompound("Clear clips", origin);
            if (!opened.succeeded())
                return false;
        }
        // Removed one at a time rather than by clearing the clip manager,
        // so that every clip produces its own removal event. Clearing
        // directly leaves observers holding clips that no longer exist.
        ProjectDocumentTransaction transaction(project_document_events_);
        for (auto& fragment : fragments) {
            if (performCapturedClipRemoval(
                    targetTrack->referenceId(), std::move(fragment), origin))
                continue;
            if (ownsCompound)
                undo_engine_.cancelCompound();
            return false;
        }
        if (ownsCompound)
            undo_engine_.endCompound();
        return true;
    }

    bool TimelineFacadeImpl::replaceAudioClipContent(
            int32_t trackIndex,
            int32_t clipId,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps,
            const std::vector<ClipMarker>& masterTrackMarkers,
            ProjectMutationOrigin origin) {
                auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return false;
        auto* clip = targetTrack->clipManager().getClip(clipId);
        if (!clip || clip->clipType != ClipType::Audio)
            return false;

        std::optional<ProjectClipFragment> before;
        const bool recordsHistory = origin == ProjectMutationOrigin::User
            || origin == ProjectMutationOrigin::Remote;
        if (recordsHistory) {
            before = captureClipFragment(trackIndex, clipId);
            if (!before)
                return false;
        }

        const auto sourcePath = filepath.empty() ? clip->filepath : filepath;
        auto reader = createAudioFileReaderFromPath(sourcePath);
        if (!reader)
            return false;

        // Resolved against the clip as it will be, not as it is: a warp may
        // reference a marker that this same call is adding.
        auto lookup = buildClipReferenceMap();
        auto target = *clip;
        target.markers = markers;
        lookup[target.referenceId] = target;
        auto resolvedWarps = resolveAudioWarpPoints(
            target, audioWarps, lookup, masterTrackMarkers, static_cast<double>(sampleRate_));

        auto replacement = std::make_unique<AudioFileSourceNode>(
            clip->sourceNodeInstanceId,
            std::move(reader),
            static_cast<double>(sampleRate_),
            std::move(resolvedWarps));
        const int64_t sourceDuration = replacement->totalLength();

        {
            ProjectDocumentTransaction transaction(project_document_events_);
            if (!targetTrack->replaceClipSourceNode(clipId, std::move(replacement)))
                return false;
            auto& clips = targetTrack->clipManager();
            clips.setClipMarkers(clipId, std::move(markers));
            clips.setAudioWarps(clipId, std::move(audioWarps));
            if (!filepath.empty()) {
                clips.setClipFilepath(clipId, filepath);
                clips.resizeClip(clipId, sourceDuration);
            }
            notifyClipChanged(trackIndex, clipId, "clip-content-changed");
            notifyTimelineChanged();
        }
        return !recordsHistory || recordReplacedClip(
            trackIndex, std::move(*before), origin, "Edit audio clip content");
    }

    bool TimelineFacadeImpl::replaceMidiClipContent(
            int32_t trackIndex,
            int32_t clipId,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            ProjectMutationOrigin origin) {
                auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return false;
        auto* clip = targetTrack->clipManager().getClip(clipId);
        if (!clip || clip->clipType != ClipType::Midi)
            return false;
        auto existing = std::dynamic_pointer_cast<MidiClipSourceNode>(
            targetTrack->getSourceNode(clip->sourceNodeInstanceId));
        if (!existing)
            return false;

        std::optional<ProjectClipFragment> before;
        const bool recordsHistory = origin == ProjectMutationOrigin::User
            || origin == ProjectMutationOrigin::Remote;
        if (recordsHistory) {
            before = captureClipFragment(trackIndex, clipId);
            if (!before)
                return false;
        }

        auto replacement = std::make_unique<MidiClipSourceNode>(
            existing->instanceId(),
            std::move(umpEvents),
            std::move(umpTickTimestamps),
            clip->tickResolution > 0 ? clip->tickResolution : existing->tickResolution(),
            existing->clipTempo(),
            static_cast<double>(sampleRate_),
            existing->tempoChanges(),
            existing->timeSignatureChanges());
        const int64_t newDuration = replacement->totalLength();

        // The node swap and the duration change are one edit.
        {
            ProjectDocumentTransaction transaction(project_document_events_);
            if (!targetTrack->replaceClipSourceNode(clipId, std::move(replacement)))
                return false;
            targetTrack->clipManager().resizeClip(clipId, newDuration);
            notifyClipChanged(trackIndex, clipId, "clip-content-changed");
            notifyTimelineChanged();
        }
        return !recordsHistory || recordReplacedClip(
            trackIndex, std::move(*before), origin, "Edit MIDI clip content");
    }

    bool TimelineFacadeImpl::appendMidiEventsToClip(int32_t trackIndex, int32_t clipId,
            std::vector<uapmd_ump_t> words, std::vector<uint64_t> ticks) {
                if (words.empty() || words.size() != ticks.size())
            return false;
        TimelineTrack* track = trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())
            ? timeline_tracks_[static_cast<size_t>(trackIndex)].get() : nullptr;
        auto* clip = track ? track->clipManager().getClip(clipId) : nullptr;
        if (!clip || clip->clipType != ClipType::Midi)
            return false;
        auto source = track->getSourceNode(clip->sourceNodeInstanceId);
        auto midi = std::dynamic_pointer_cast<MidiClipSourceNode>(source);
        if (!midi)
            return false;
        struct TimedUmp {uint64_t tick{
                };
            std::vector<uapmd_ump_t> words;
            bool recorded{};
        };
        std::vector<TimedUmp> events;
        const auto appendEvents = [&events](const std::vector<uapmd_ump_t>& sourceWords,
                                            const std::vector<uint64_t>& sourceTicks,
                                            bool recorded) {
            for (size_t offset = 0; offset < sourceWords.size();) {
                const auto wordCount = std::max<size_t>(1,
                    umppi::umpSizeInInts(static_cast<uint8_t>(sourceWords[offset] >> 28)));
                if (offset + wordCount > sourceWords.size() || offset >= sourceTicks.size())
                    return false;
                events.push_back({
                    sourceTicks[offset],
                    std::vector<uapmd_ump_t>(sourceWords.begin() + static_cast<std::ptrdiff_t>(offset),
                                             sourceWords.begin() + static_cast<std::ptrdiff_t>(offset + wordCount)),
                    recorded});
                offset += wordCount;
            }
            return true;
        };
        if (!appendEvents(midi->umpEvents(), midi->eventTimestampsTicks(), false) ||
            !appendEvents(words, ticks, true))
            return false;
        // At an identical tick preserve existing clip data before newly
        // recorded input; stable_sort retains each stream's event order.
        std::stable_sort(events.begin(), events.end(), [](const TimedUmp& a, const TimedUmp& b) {
            if (a.tick != b.tick)
                return a.tick < b.tick;
            return !a.recorded && b.recorded;
        });
        std::vector<uapmd_ump_t> allWords;
        std::vector<uint64_t> allTicks;
        for (const auto& event : events) {
            allWords.insert(allWords.end(), event.words.begin(), event.words.end());
            allTicks.insert(allTicks.end(), event.words.size(), event.tick);
        }
        return replaceMidiClipContent(
            trackIndex,
            clipId,
            std::move(allWords),
            std::move(allTicks),
            ProjectMutationOrigin::User);
    }

    ClipReferenceMap TimelineFacadeImpl::buildClipReferenceMap() const {
        ClipReferenceMap lookup;
        auto collect = [&lookup](const std::shared_ptr<TimelineTrack>& track) {
            if (!track)
                return;
            for (auto& clip : track->clipManager().getAllClips())
                lookup[clip.referenceId] = std::move(clip);
        };
        collect(master_timeline_track_);
        for (const auto& track : timeline_tracks_)
            collect(track);
        return lookup;
    }

    bool TimelineFacadeImpl::notifyClipChanged(int32_t trackIndex, int32_t clipId, std::string type) {
        auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return false;
        auto* clip = targetTrack->clipManager().getClip(clipId);
        if (!clip)
            return false;
        emitClipChanged(*targetTrack, *clip, std::move(type));
        if (clip->clipType == ClipType::Midi)
            emitMasterTrackChanged("master-track-content-changed");
        return true;
    }

    bool TimelineFacadeImpl::setClipEnabled(
            int32_t trackIndex,
            int32_t clipId,
            bool enabled,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipEnabledProperty>(
            trackIndex, clipId, enabled, origin);
    }

    bool TimelineFacadeImpl::setClipAnchor(
            int32_t trackIndex,
            int32_t clipId,
            const TimeReference& anchor,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipAnchorProperty>(
            trackIndex, clipId, anchor, origin);
    }

    bool TimelineFacadeImpl::setClipGain(
            int32_t trackIndex,
            int32_t clipId,
            double gain,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipGainProperty>(
            trackIndex, clipId, gain, origin);
    }

    bool TimelineFacadeImpl::setClipMuted(
            int32_t trackIndex,
            int32_t clipId,
            bool muted,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipMutedProperty>(
            trackIndex, clipId, muted, origin);
    }

    bool TimelineFacadeImpl::resizeClip(
            int32_t trackIndex,
            int32_t clipId,
            int64_t newDurationSamples,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipDurationProperty>(
            trackIndex, clipId, newDurationSamples, origin);
    }

    bool TimelineFacadeImpl::setClipName(
            int32_t trackIndex,
            int32_t clipId,
            const std::string& name,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipNameProperty>(
            trackIndex, clipId, name, origin);
    }

    bool TimelineFacadeImpl::setClipFilepath(
            int32_t trackIndex,
            int32_t clipId,
            const std::string& filepath,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipFilepathProperty>(
            trackIndex, clipId, filepath, origin);
    }

    bool TimelineFacadeImpl::setClipNeedsFileSave(
            int32_t trackIndex,
            int32_t clipId,
            bool needsSave,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipNeedsFileSaveProperty>(
            trackIndex, clipId, needsSave, origin);
    }

    bool TimelineFacadeImpl::setClipMarkers(
            int32_t trackIndex,
            int32_t clipId,
            std::vector<ClipMarker> markers,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipMarkersProperty>(
            trackIndex, clipId, std::move(markers), origin);
    }

    bool TimelineFacadeImpl::setClipAudioWarps(
            int32_t trackIndex,
            int32_t clipId,
            std::vector<AudioWarpPoint> audioWarps,
            ProjectMutationOrigin origin) {
                return executeClipProperty<ClipAudioWarpsProperty>(
            trackIndex, clipId, std::move(audioWarps), origin);
    }

    bool TimelineFacadeImpl::clipEnabled(int32_t trackIndex, int32_t clipId) const {
        const auto* targetTrack = resolveTrack(trackIndex);
        const auto* clip = targetTrack ? targetTrack->clipManager().getClip(clipId) : nullptr;
        return clip ? clip->enabled : false;
    }

} // namespace uapmd
