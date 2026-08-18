#include "TimelineFacadeImpl.hpp"

// Audio rendering and timeline analysis: feeding clip sources into track
// buffers, render offsets, tempo/marker snapshots and content bounds.

namespace uapmd {
    void TimelineFacadeImpl::processTracksAudio(AudioProcessContext& process, SequenceProcessContext& targetSequence) {
        // Protect the snapshot for the duration of this callback so that
        // tracks added or removed on the UI thread cannot destroy TrackList
        // elements while we are iterating them.
        auto snapshot = timeline_tracks_snapshot_.protect();

        auto wrapToLoopRange = [this](int64_t samplePosition) -> int64_t {
            if (!timeline_.loopEnabled || timeline_.loopEnd.samples <= timeline_.loopStart.samples)
                return samplePosition;
            const auto loopLength = timeline_.loopEnd.samples - timeline_.loopStart.samples;
            if (samplePosition < timeline_.loopStart.samples)
                return samplePosition;
            return timeline_.loopStart.samples +
                ((samplePosition - timeline_.loopStart.samples) % loopLength);
        };

        // Published by the model thread; loading it is the whole cost here.
        // Building it walks and sorts every master clip, which must never
        // happen on this thread.
        const auto masterSnapshotGuard = master_track_snapshot_.protect();
        const auto& masterSnapshot = *masterSnapshotGuard;
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

        const bool offlineRenderPlaying = engine_.offlineRendering();
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
        // Offline source nodes need the same private running transport as
        // the plugins they feed. Keep the shared application timeline
        // stopped, but allow audio sources and MIDI clips to produce the
        // content being frozen.
        renderTransport.isPlaying =
            timeline_.isPlaying || offlineRenderPlaying;
        renderTransport.playheadPosition.samples = wrapToLoopRange(
            (timeline_.isPlaying || offlineRenderPlaying ||
             renderPlayheadRaw != audiblePlayheadSamples) ?
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
        // Offline render transport is private processing state. Plugins
        // must see a running transport to render correctly, but the shared
        // application timeline must remain stopped.
        masterCtx.isPlaying(
            timeline_.isPlaying || offlineRenderPlaying);
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
                (timeline_.isPlaying || offlineRenderPlaying ||
                 renderPlayheadRaw != audiblePlayheadSamples) ?
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

    void TimelineFacadeImpl::onTrackAdded(
            uint32_t outputChannels,
            double sampleRate,
            uint32_t bufferSizeInFrames,
            int32_t insertionIndex) {
                sampleRate_ = static_cast<int32_t>(sampleRate);
        bufferSizeInFrames_ = bufferSizeInFrames;
        // Snapshot times are derived from the sample rate.
        rebuildMasterTrackSnapshot();
        master_timeline_track_->reconfigureBuffers(0, bufferSizeInFrames);

        std::string trackReferenceId;
        if (!pending_track_reference_id_.empty()) {
            trackReferenceId = std::move(pending_track_reference_id_);
            pending_track_reference_id_.clear();
            reserveTrackReferenceId(trackReferenceId);
        } else
            trackReferenceId = std::format("track_{}", next_timeline_track_reference_++);
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
                    } else {value = static_cast<double>(rawValue) / UINT32_MAX;
                    }
                    engine_.setParameterValue(instanceId, static_cast<int32_t>(paramIdx), value);
                }
            });

        const auto trackIndex = insertionIndex < 0
            ? static_cast<int32_t>(timeline_tracks_.size())
            : insertionIndex;
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) > timeline_tracks_.size())
            return;
        const auto eventTrackId = newTrack->referenceId();
        timeline_tracks_.insert(
            timeline_tracks_.begin() + trackIndex,
            std::move(newTrack));
        rebuildTrackSnapshot();

        ProjectDocumentEvent event(ProjectDocumentEventKind::TrackAdded, "track-added");
        event.setTrackId(eventTrackId)
            .setTrackIndex(trackIndex);
        emitProjectDocumentEvent(std::move(event));
    }

    void TimelineFacadeImpl::onTrackRemoved(size_t trackIndex) {        if (trackIndex < timeline_tracks_.size()) {
            const auto eventTrackId = timeline_tracks_[trackIndex]->referenceId();
            timeline_tracks_.erase(timeline_tracks_.begin() + static_cast<long>(trackIndex));
            rebuildTrackSnapshot();

            ProjectDocumentEvent event(ProjectDocumentEventKind::TrackRemoved, "track-removed");
            event.setTrackId(eventTrackId)
                .setTrackIndex(static_cast<int32_t>(trackIndex));
            emitProjectDocumentEvent(std::move(event));
        }
    }

    void TimelineFacadeImpl::onTrackGraphChanged(int32_t trackIndex) {
        if (suppress_plugin_graph_notifications_ != 0)
            return;
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

    uint32_t TimelineFacadeImpl::maxTrackLatencyInSamples() {
        uint32_t maxLatency = 0;
        auto& tracks = engine_.tracks();
        for (size_t i = 0; i < tracks.size(); ++i)
            maxLatency = std::max(maxLatency,
                                  engine_.trackRenderLeadInSamples(static_cast<int32_t>(i)));
        return maxLatency;
    }

    uint32_t TimelineFacadeImpl::trackRenderOffsetInSamples(int32_t trackIndex) {
        if (trackIndex < 0)
            return 0;
        auto trackLatency = engine_.trackRenderLeadInSamples(trackIndex);
        auto masterLatency = engine_.masterTrackRenderLeadInSamples();
        return masterLatency + trackLatency;
    }

    uint32_t TimelineFacadeImpl::masterTrackRenderOffsetInSamples() {
        return engine_.masterTrackRenderLeadInSamples();
    }

    bool TimelineFacadeImpl::trackHasLiveInput(int32_t trackIndex) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= timeline_tracks_.size())
            return false;
        auto* track = timeline_tracks_[static_cast<size_t>(trackIndex)].get();
        return track ? track->hasDeviceInputSource() : false;
    }

    TimelineFacade::MasterTrackSnapshot TimelineFacadeImpl::computeMasterTrackSnapshot() const {
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

        // Regular tracks can never carry meaningful tempo/time-signature data of their own
        // (see MidiClipReader::stripToFlatTempo / TrackImporter::importMidiFile) -- the
        // master track is always the sole source, so no fallback search is needed here.
        appendTrackMeta(master_timeline_track_);

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

    void TimelineFacadeImpl::appendMidiNodeMetaToSnapshot(MasterTrackSnapshot& snapshot,
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

    TimelineFacade::ContentBounds TimelineFacadeImpl::calculateContentBounds() const {
        ContentBounds bounds;
        const double sr = std::max(1.0, static_cast<double>(sampleRate_));
        for (size_t trackIndex = 0;
             trackIndex < timeline_tracks_.size();
             ++trackIndex) {
            const auto trackBounds =
                calculateTrackContentBounds(
                    static_cast<int32_t>(trackIndex));
            if (!trackBounds.hasContent)
                continue;
            if (!bounds.hasContent ||
                trackBounds.firstSample < bounds.firstSample)
                bounds.firstSample = trackBounds.firstSample;
            if (!bounds.hasContent ||
                trackBounds.lastSample > bounds.lastSample)
                bounds.lastSample = trackBounds.lastSample;
            bounds.hasContent = true;
        }
        if (bounds.hasContent) {
            bounds.firstSeconds =
                static_cast<double>(bounds.firstSample) / sr;
            bounds.lastSeconds =
                static_cast<double>(bounds.lastSample) / sr;
        }
        return bounds;
    }

    TimelineFacade::ContentBounds TimelineFacadeImpl::calculateTrackContentBounds(int32_t trackIndex) const {
        ContentBounds bounds;
        if (trackIndex < 0 ||
            static_cast<size_t>(trackIndex) >= timeline_tracks_.size())
            return bounds;

        const double sr = std::max(1.0, static_cast<double>(sampleRate_));
        const auto& trackPtr =
            timeline_tracks_[static_cast<size_t>(trackIndex)];
        if (!trackPtr)
            return bounds;

        const auto clips = trackPtr->clipManager().getAllClips();
        std::unordered_map<std::string, const ClipData*> clipReferenceMap;
        clipReferenceMap.reserve(clips.size());
        for (const auto& clip : clips)
            clipReferenceMap[clip.referenceId] = &clip;

        for (const auto& clip : clips) {
            const auto absolute =
                clip.getAbsolutePosition(clipReferenceMap);
            const int64_t startSample = absolute.samples;
            const int64_t durationSamples =
                std::max<int64_t>(0, clip.durationSamples);
            const int64_t endSample =
                startSample > 0 &&
                    durationSamples >
                        std::numeric_limits<int64_t>::max() - startSample
                ? std::numeric_limits<int64_t>::max()
                : startSample + durationSamples;

            if (!bounds.hasContent || startSample < bounds.firstSample) {
                bounds.firstSample = startSample;
                bounds.firstSeconds = static_cast<double>(startSample) / sr;
            }
            if (!bounds.hasContent || endSample > bounds.lastSample) {
                bounds.lastSample = endSample;
                bounds.lastSeconds = static_cast<double>(endSample) / sr;
            }
            bounds.hasContent = true;
        }
        return bounds;
    }

    std::optional<std::vector<TimelineFacade::MidiNotePreview>> TimelineFacadeImpl::getMidiClipNotes(int32_t trackIndex, int32_t clipId) const {
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
                    } else {auto it = activeNoteIndices.find(key);
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
                    } else {auto it = activeNoteIndices.find(key);
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

    void TimelineFacadeImpl::resolveAllClipAnchors() {        struct ClipRecord {ClipManager* manager{
                };
            ClipData clip{};
        };
        std::unordered_map<std::string, ClipRecord> records;
        auto collect = [&records](TimelineTrack* track) {
            if (!track)
                return;
            for (const auto& clip : track->clipManager().getAllClips())
                records.emplace(
                    clip.referenceId,
                    ClipRecord{&track->clipManager(), clip});
        };
        collect(master_timeline_track_.get());
        for (const auto& track : timeline_tracks_)
            collect(track.get());

        std::unordered_map<std::string, TimelinePosition> resolved;
        std::unordered_set<std::string> resolving;
        std::function<TimelinePosition(const std::string&)> resolve =
            [&](const std::string& referenceId) -> TimelinePosition {
                if (auto found = resolved.find(referenceId); found != resolved.end())
                    return found->second;
                auto found = records.find(referenceId);
                if (found == records.end())
                    return {};

                const auto reference = found->second.clip.timeReference(sampleRate_);
                if (reference.referenceId.empty()) {
                    auto position = TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                    resolved[referenceId] = position;
                    return position;
                }
                if (!resolving.insert(referenceId).second) {
                    auto position = TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                    resolved[referenceId] = position;
                    return position;
                }

                auto anchor = records.find(reference.referenceId);
                if (anchor == records.end()) {
                    resolving.erase(referenceId);
                    auto position = TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                    resolved[referenceId] = position;
                    return position;
                }
                auto position = resolve(reference.referenceId);
                if (reference.type == TimeReferenceType::ContainerEnd)
                    position.samples += anchor->second.clip.durationSamples;
                position = position
                    + TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                resolving.erase(referenceId);
                resolved[referenceId] = position;
                return position;
            };

        for (const auto& [referenceId, record] : records)
            record.manager->setClipPosition(record.clip.clipId, resolve(referenceId));
    }

    void TimelineFacadeImpl::applyAuthoritativeTempoMapToMusicalClips() {
        auto tempoChanges = MidiClipReader::applyAuthoritativeTempoMapToMusicalClips(master_timeline_track_, timeline_tracks_);
        if (!tempoChanges.empty())
            timeline_.tempo = tempoChanges.front().bpm;
    }

    void TimelineFacadeImpl::rebuildMasterTrackSnapshot() {
        master_track_snapshot_.publish(
            std::make_unique<const MasterTrackSnapshot>(computeMasterTrackSnapshot()));
    }

    // External callers read the same snapshot the audio thread sees, so a
    // missed invalidation shows up in the UI rather than only in playback.
    TimelineFacade::MasterTrackSnapshot TimelineFacadeImpl::buildMasterTrackSnapshot() {
        return *master_track_snapshot_.currentOnPublisherThread();
    }

    void TimelineFacadeImpl::rebuildTrackSnapshot() {
        timeline_tracks_snapshot_.publish(
            std::make_unique<const TrackList>(timeline_tracks_));
    }

} // namespace uapmd
