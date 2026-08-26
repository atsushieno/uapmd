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
        auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack) {
            result.error = "Invalid track index";
            return result;
        }

        auto clipInfo = MidiClipReader::readAnyFormat(filepath);
        if (!clipInfo.success) {
            result.error = clipInfo.error;
            return result;
        }

        // Importing onto the master track itself: the file's meta events are
        // already destined for where they would be split off to, so the clip
        // goes there whole rather than being separated into two.
        if (trackIndex == kMasterTrackIndex) {
            return recordAddedClip(
                kMasterTrackIndex,
                addMidiClipToTimelineTrack(
                    *targetTrack,
                    position,
                    filepath,
                    std::move(clipInfo.ump_data),
                    std::move(clipInfo.ump_tick_timestamps),
                    clipInfo.tick_resolution,
                    clipInfo.tempo,
                    std::move(clipInfo.tempo_changes),
                    std::move(clipInfo.time_signature_changes),
                    std::filesystem::path(filepath).stem().string(),
                    false,
                    false),
                origin);
        }

        auto separated = MidiClipReader::separateMasterTrackEvents(std::move(clipInfo));
        auto& musicalClip = separated.musicalClip;
        auto& track = *targetTrack;
        // Only worth a step when this import records history and adds both a
        // musical and a master-track clip; opening one for a project load
        // would make the whole load look like a pending history scope.
        const bool recordsHistory = origin == ProjectMutationOrigin::User
            || origin == ProjectMutationOrigin::Remote;
        std::optional<ScopedCommandStep> step;
        if (recordsHistory
            && separated.hasMasterTrackClip()
            && !command_manager_.state().compoundOpen) {
            step.emplace(command_manager_, "Import MIDI file", origin);
            if (!step->opened()) {
                result.error = step->error();
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
        if (!result.success)
            return result;

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
                result.success = false;
                result.error = masterResult.error.empty()
                    ? "Could not add the imported master MIDI clip"
                    : std::move(masterResult.error);
                return result;
            }
        }
        if (step)
            step->commit();
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
            ProjectMutationOrigin origin) {
    // resolveTrack, rather than a bounds check on timeline_tracks_, so that
    // kMasterTrackIndex reaches the master track here the same way it does in
    // removeClipFromTrack and the clip property commands.
    auto* targetTrack = resolveTrack(trackIndex);
    if (!targetTrack) {
        ClipAddResult result;
        result.error = "Invalid track index";
        return result;
    }
    return recordAddedClip(
        trackIndex,
        addMidiClipToTimelineTrack(
            *targetTrack,
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

        // A caller clearing several tracks owns the step; this opens one only
        // when it is the whole action.
        std::optional<ScopedCommandStep> step;
        if (!command_manager_.state().compoundOpen) {
            step.emplace(command_manager_, "Clear clips", origin);
            if (!step->opened())
                return false;
        }
        // Removed one at a time rather than by clearing the clip manager,
        // so that every clip produces its own removal event. Clearing
        // directly leaves observers holding clips that no longer exist.
        ProjectDocumentTransaction transaction(project_document_events_);
        for (auto& fragment : fragments) {
            if (!performCapturedClipRemoval(
                    targetTrack->referenceId(), std::move(fragment), origin))
                return false;
        }
        if (step)
            step->commit();
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











    bool TimelineFacadeImpl::clipEnabled(int32_t trackIndex, int32_t clipId) const {
        const auto* targetTrack = resolveTrack(trackIndex);
        const auto* clip = targetTrack ? targetTrack->clipManager().getClip(clipId) : nullptr;
        return clip ? clip->enabled : false;
    }

} // namespace uapmd
