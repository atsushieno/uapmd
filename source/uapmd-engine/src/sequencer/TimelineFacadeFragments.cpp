#include "TimelineFacadeImpl.hpp"

// Detached clip and track representations, and the structural mutations built
// on them: capture, re-attach, add and remove.

namespace uapmd {
    void TimelineFacadeImpl::captureTrackFragment(int32_t trackIndex, TrackFragmentCallback callback) {
        if (!callback)
            return;
        // Same constraint as capturing a clip: extensions archive plug-in
        // state here, which ARA forbids while the document is being edited.
        if (project_document_events_.inTransaction()) {
            callback(std::nullopt,
                     "captureTrackFragment must not be called inside a document transaction");
            return;
        }

        auto* timelineTrack = resolveTrack(trackIndex);
        auto* sequencerTrack = resolveSequencerTrack(trackIndex);
        if (!timelineTrack || !sequencerTrack) {
            callback(std::nullopt, "Invalid track index");
            return;
        }

        auto fragment = std::make_shared<ProjectTrackFragment>();
        fragment->referenceId = timelineTrack->referenceId();
        fragment->volume = sequencerTrack->trackGain();
        fragment->muted = sequencerTrack->muted();
        fragment->solo = sequencerTrack->solo();

        // Graph topology, held by value. The .graph.json a project save
        // writes is an artifact of saving; the serialization itself
        // produces bytes.
        auto* provider = audio_graph_provider_registry_.get(sequencerTrack->graph());
        if (!provider) {
            callback(std::nullopt, "The track's graph provider is unavailable");
            return;
        }
        auto graphData = createSerializedProjectGraph(
            *provider,
            sequencerTrack->orderedInstanceIds(),
            sequencerTrack->graph(),
            [this](int32_t instanceId) { return engine_.getPluginInstance(instanceId); },
            nullptr);
        if (!graphData) {
            callback(std::nullopt, "Could not capture the track graph");
            return;
        }
        fragment->graphType = provider->id();
        if (!provider->saveProjectGraph(graphData.get(), fragment->graphBytes)) {
            callback(std::nullopt, "Could not serialize the track graph");
            return;
        }

        // Plugin descriptors now; their state is read asynchronously below.
        auto pluginInstanceIds = std::make_shared<std::vector<int32_t>>();
        for (int32_t instanceId : sequencerTrack->orderedInstanceIds()) {
            auto* instance = engine_.getPluginInstance(instanceId);
            if (!instance)
                continue;
            ProjectTrackPluginFragment plugin;
            if (auto* node = sequencerTrack->graph().getPluginNode(instanceId))
                plugin.nodeId = node->nodeId();
            plugin.pluginId = instance->pluginId();
            plugin.format = instance->formatName();
            plugin.displayName = instance->displayName();
            const auto group = sequencerTrack->getInstanceGroup(instanceId);
            plugin.groupIndex = group == 0xFF ? -1 : static_cast<int32_t>(group);
            fragment->plugins.push_back(std::move(plugin));
            pluginInstanceIds->push_back(instanceId);
        }

        for (const auto& clip : timelineTrack->clipManager().getAllClips()) {
            auto clipFragment = captureClipFragment(trackIndex, clip.clipId);
            if (!clipFragment) {
                callback(std::nullopt,
                         std::format("Failed to capture clip {} of the track", clip.referenceId));
                return;
            }
            fragment->clips.push_back(std::move(*clipFragment));
        }

        for (auto* extension : projectSerializationExtensionsSnapshot()) {
            std::vector<uint8_t> state;
            std::string extensionError;
            if (!extension->captureTrackFragmentState(fragment->referenceId, state, extensionError)) {
                callback(std::nullopt,
                         std::format("Extension {} could not capture its state for track {}: {}",
                                     extension->extensionId(), fragment->referenceId, extensionError));
                return;
            }
            if (!state.empty())
                fragment->extensionState[std::string(extension->extensionId())] = std::move(state);
        }

        // Plugin state is callback-based, so the remaining work is a chain
        // rather than a loop. This is the reason capture cannot simply
        // return a fragment the way the clip version does.
        auto sharedCallback = std::make_shared<TrackFragmentCallback>(std::move(callback));
        auto step = std::make_shared<std::function<void(size_t)>>();
        *step = [this, fragment, pluginInstanceIds, sharedCallback, step](size_t index) {
            if (index >= pluginInstanceIds->size()) {
                (*sharedCallback)(std::move(*fragment), std::string{});
                return;
            }
            auto* instance = engine_.getPluginInstance((*pluginInstanceIds)[index]);
            if (!instance) {
                (*step)(index + 1);
                return;
            }
            instance->requestState(
                StateContextType::Project, false, nullptr,
                [fragment, sharedCallback, step, index](
                    std::vector<uint8_t> state, std::string error, void*) mutable {if (!error.empty()) {
                        (*sharedCallback)(std::nullopt, std::move(error));
                        return;
                    }
                    fragment->plugins[index].state = std::move(state);
                    (*step)(index + 1);
                });
        };
        (*step)(0);
    }

    void TimelineFacadeImpl::attachTrackFragment(
            const ProjectTrackFragment& fragment,
            ProjectTrackAttachOptions options,
            TrackAttachCallback callback) {
                if (!callback)
            return;
        // Copied because the chain below outlives this call.
        auto source = std::make_shared<ProjectTrackFragment>(fragment);
        auto sharedCallback = std::make_shared<TrackAttachCallback>(std::move(callback));

        const auto graphType = options.includePlugins ? source->graphType : std::string{};
        if (!graphType.empty() && !audio_graph_provider_registry_.get(graphType)) {
            (*sharedCallback)(
                -1,
                std::format(
                    "Could not create graph type {} while attaching the track",
                    graphType));
            return;
        }

        struct AttachmentState {
            std::unique_ptr<PreparedSequencerTrack> prepared;
            int32_t publishedTrackIndex{-1};
            bool completed{false};
        };
        auto state = std::make_shared<AttachmentState>();
        state->prepared = engine_.prepareTrack(graphType);
        if (!state->prepared) {
            (*sharedCallback)(-1, "Failed to prepare track");
            return;
        }

        auto fail = std::make_shared<std::function<void(std::string)>>();
        *fail = [this, state, sharedCallback](std::string error) {
            if (state->completed)
                return;
            state->completed = true;
            state->prepared.reset();
            if (state->publishedTrackIndex >= 0
                && !engine_.removeTrack(state->publishedTrackIndex))
                error += " The partially attached track could not be removed.";
            (*sharedCallback)(-1, std::move(error));
        };

        auto& preparedTrack = state->prepared->track();
        preparedTrack.trackGain(source->volume);
        preparedTrack.muted(source->muted);
        preparedTrack.solo(source->solo);

        // Applied only after every plug-in has been created, configured and
        // restored on the detached track. Publishing and synchronous
        // document restoration share one transaction, so no asynchronous
        // observer can see the track filling in.
        auto finish = [this, source, options, state, sharedCallback]() {
            std::string error;
            ProjectDocumentTransaction transaction(project_document_events_);

            if (options.idPolicy == ProjectObjectIdPolicy::Restore)
                pending_track_reference_id_ = source->referenceId;
            state->publishedTrackIndex = engine_.publishPreparedTrack(
                std::move(state->prepared), options.insertionIndex);
            pending_track_reference_id_.clear();
            if (state->publishedTrackIndex < 0)
                error = "Failed to publish the prepared track";

            if (error.empty() && options.includeClips) {
                for (const auto& clipFragment : source->clips) {
                    auto result = attachClipFragment(
                        state->publishedTrackIndex, clipFragment, options.idPolicy);
                    if (!result.success) {
                        error = result.error.empty()
                            ? "Failed to restore a clip while attaching the track"
                            : std::move(result.error);
                        break;
                    }
                }
            }

            if (error.empty()) {
                auto* timelineTrack = resolveTrack(state->publishedTrackIndex);
                if (!timelineTrack) {
                    error = "The created timeline track is unavailable";
                } else {for (auto* extension : projectSerializationExtensionsSnapshot()) {
                        const auto it = source->extensionState.find(std::string(extension->extensionId()));
                        static const std::vector<uint8_t> kNoState{};
                        const auto& extensionState = it == source->extensionState.end()
                            ? kNoState : it->second;
                        std::string extensionError;
                        if (!extension->restoreTrackFragmentState(
                                timelineTrack->referenceId(), extensionState, extensionError)) {
                            error = std::format(
                                "Extension {} failed to restore state for track {}: {}",
                                extension->extensionId(),
                                timelineTrack->referenceId(),
                                extensionError);
                            break;
                        }
                    }
                }
            }

            if (!error.empty()) {
                if (state->publishedTrackIndex >= 0
                    && !engine_.removeTrack(state->publishedTrackIndex))
                    error += " The partially attached track could not be removed.";
                state->publishedTrackIndex = -1;
                state->completed = true;
                (*sharedCallback)(-1, std::move(error));
                return;
            }
            if (state->completed)
                return;
            state->completed = true;
            (*sharedCallback)(state->publishedTrackIndex, std::string{});
        };

        auto finishHolder = std::make_shared<std::function<void()>>(
            [this, source, options, state, finish = std::move(finish), fail]() mutable {if (options.includePlugins && !source->graphBytes.empty()) {
                    auto* provider = audio_graph_provider_registry_.get(source->graphType);
                    auto metadata = UapmdProjectPluginGraphData::create();
                    if (!provider || !state->prepared || !metadata) {
                        (*fail)("Could not resolve the captured track graph");
                        return;
                    }
                    metadata->graphType(source->graphType);
                    auto graphData = loadSerializedProjectGraph(
                        *provider, *metadata, source->graphBytes);
                    if (!graphData || !provider->deserializeRuntimeGraph(
                            graphData.get(),
                            state->prepared->track().graph(),
                            state->prepared->track().orderedInstanceIds())) {
                        (*fail)("Could not restore the captured track graph topology");
                        return;
                    }
                }
                finish();
            });
        if (!options.includePlugins) {
            (*finishHolder)();
            return;
        }
        if (source->plugins.empty()) {
            (*finishHolder)();
            return;
        }

        // Instantiating a plugin is callback-based, so plugins are added
        // one at a time rather than in a loop.
        auto step = std::make_shared<std::function<void(size_t)>>();
        *step = [this, source, options, state, finishHolder, step, fail](size_t index) {
            if (index >= source->plugins.size()) {
                (*finishHolder)();
                return;
            }
            auto& plugin = source->plugins[index];
            // Restore reuses the captured node identity so that anything
            // keyed by it reconnects; Mint leaves it empty and a fresh one
            // is derived from the new instance.
            auto restoreNodeId = options.idPolicy == ProjectObjectIdPolicy::Restore
                ? plugin.nodeId
                : std::string{};
            // addPluginToTrack takes non-const references.
            auto format = plugin.format;
            auto pluginId = plugin.pluginId;
            engine_.addPluginToPreparedTrack(
                *state->prepared, format, pluginId,
                [this, source, options, state, step, index, fail](
                    int32_t instanceId, std::string error) {
                    auto& added = source->plugins[index];
                    if (!error.empty() || instanceId < 0) {
                        (*fail)(std::format(
                            "Failed to instantiate {} while attaching the track: {}",
                            added.displayName.empty() ? added.pluginId : added.displayName,
                            error));
                        return;
                    }
                    if (added.groupIndex >= 0 && added.groupIndex <= 15) {
                        const auto restoredGroup = static_cast<uint8_t>(added.groupIndex);
                        const auto& instanceIds =
                            state->prepared->track().orderedInstanceIds();
                        const auto groupInUse = std::ranges::any_of(
                            instanceIds,
                            [state, instanceId, restoredGroup](int32_t otherInstanceId) {
                                return otherInstanceId != instanceId
                                    && state->prepared->track().getInstanceGroup(otherInstanceId)
                                        == restoredGroup;
                            });
                        if (groupInUse) {
                            (*fail)(std::format(
                                "Failed to restore the MIDI group for {}",
                                added.displayName.empty() ? added.pluginId : added.displayName));
                            return;
                        }
                        state->prepared->track().setInstanceGroup(
                            instanceId, restoredGroup);
                    }

                    auto* instance = state->prepared->pluginInstance(instanceId);
                    if (!instance) {
                        (*fail)(std::format(
                            "The restored plugin instance for {} is unavailable",
                            added.displayName.empty() ? added.pluginId : added.displayName));
                        return;
                    }
                    if (!options.includePluginState || added.state.empty()) {
                        (*step)(index + 1);
                        return;
                    }
                    instance->loadState(
                        added.state, StateContextType::Project, false, nullptr,
                        [this, step, index, fail,
                         displayName = added.displayName,
                         pluginId = added.pluginId](std::string loadError, void*) {
                            if (!loadError.empty()) {
                                (*fail)(std::format(
                                    "Failed to restore state for {} while attaching the track: {}",
                                    displayName.empty() ? pluginId : displayName,
                                    loadError));
                                return;
                            }
                            (*step)(index + 1);
                        });
                },
                std::move(restoreNodeId));
        };
        (*step)(0);
    }

    void TimelineFacadeImpl::addEmptyTrack(
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) {
                if (!callback)
            return;
        const bool recordsHistory = origin == ProjectMutationOrigin::User
            || origin == ProjectMutationOrigin::Remote;
        const auto undoState = undo_engine_.state();
        if (recordsHistory && undoState.busy && !undoState.compoundOpen) {
            callback(-1, "An undo history operation is already pending");
            return;
        }

        const auto trackIndex = engine_.addEmptyTrack();
        if (trackIndex < 0) {
            callback(-1, "Failed to create track");
            return;
        }
        if (!recordsHistory) {
            callback(trackIndex, {});
            return;
        }

        recordTrackAddition(trackIndex, origin, std::move(callback));
    }

    void TimelineFacadeImpl::recordTrackAddition(
            int32_t trackIndex,
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) {
                if (!callback)
            return;
        const bool recordsHistory = origin == ProjectMutationOrigin::User
            || origin == ProjectMutationOrigin::Remote;
        if (!recordsHistory) {
            callback(trackIndex, {});
            return;
        }
        if (!resolveTrack(trackIndex)) {
            callback(-1, "Invalid track index");
            return;
        }
        const auto undoState = undo_engine_.state();
        if (undoState.busy && !undoState.compoundOpen) {
            engine_.removeTrack(trackIndex);
            callback(-1, "An undo history operation is already pending");
            return;
        }

        pending_plugin_mutations_.fetch_add(1, std::memory_order_acq_rel);
        TrackAttachCallback finishCallback =
            [this, callback = std::move(callback)](
                int32_t completedTrackIndex, std::string error) mutable {
                    pending_plugin_mutations_.fetch_sub(1, std::memory_order_acq_rel);
                callback(completedTrackIndex, std::move(error));
            };

        captureTrackFragment(
            trackIndex,
            [this, trackIndex, origin, callback = std::move(finishCallback)](
                std::optional<ProjectTrackFragment> fragment,
                std::string error) mutable {
                    auto finish = [this, trackIndex, origin,
                               callback = std::move(callback),
                               fragment = std::move(fragment),
                               error = std::move(error)]() mutable {if (!fragment) {
                        engine_.removeTrack(trackIndex);
                        callback(
                            -1,
                            error.empty()
                                ? "Could not capture the added track for undo history"
                                : std::move(error));
                        return;
                    }
                    auto operation = makeTrackStructureOperation(
                        TrackStructureUndoOperation::InitialDirection::Addition,
                        trackIndex,
                        std::move(*fragment));
                    undo_engine_.recordPerformed(
                        std::move(operation),
                        origin,
                        [this, trackIndex, callback = std::move(callback)](
                            ProjectUndoResult result) mutable {if (!result.succeeded()) {
                                engine_.removeTrack(trackIndex);
                                callback(-1, std::move(result.error));
                                return;
                            }
                            callback(trackIndex, {});
                        });
                };
                dispatchToModelThread(std::move(finish));
            });
    }

    void TimelineFacadeImpl::removeTrack(
            int32_t trackIndex,
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) {
                if (!callback)
            return;
        const bool recordsHistory = origin == ProjectMutationOrigin::User
            || origin == ProjectMutationOrigin::Remote;
        auto* track = resolveTrack(trackIndex);
        if (!track || trackIndex == kMasterTrackIndex) {
            callback(-1, "Invalid track index");
            return;
        }
        if (!recordsHistory) {
            if (engine_.removeTrack(trackIndex))
                callback(trackIndex, {});
            else
                callback(-1, "Failed to remove track");
            return;
        }
        const auto undoState = undo_engine_.state();
        if (undoState.busy && !undoState.compoundOpen) {
            callback(-1, "An undo history operation is already pending");
            return;
        }

        pending_plugin_mutations_.fetch_add(1, std::memory_order_acq_rel);
        TrackAttachCallback finishCallback =
            [this, callback = std::move(callback)](
                int32_t completedTrackIndex, std::string error) mutable {
                    pending_plugin_mutations_.fetch_sub(1, std::memory_order_acq_rel);
                callback(completedTrackIndex, std::move(error));
            };

        const auto expectedReferenceId = track->referenceId();
        captureTrackFragment(
            trackIndex,
            [this, trackIndex, origin, expectedReferenceId,
             callback = std::move(finishCallback)](
                std::optional<ProjectTrackFragment> fragment,
                std::string error) mutable {
                    auto finish = [this, trackIndex, origin, expectedReferenceId,
                               callback = std::move(callback),
                               fragment = std::move(fragment),
                               error = std::move(error)]() mutable {if (!fragment) {
                        callback(
                            -1,
                            error.empty() ? "Could not capture the track" : std::move(error));
                        return;
                    }
                    if (fragment->referenceId != expectedReferenceId
                        || trackIndexForPersistentId(expectedReferenceId) != trackIndex) {
                        callback(-1, "The track changed position while it was being captured");
                        return;
                    }

                    auto operation = makeTrackStructureOperation(
                        TrackStructureUndoOperation::InitialDirection::Removal,
                        trackIndex,
                        std::move(*fragment));
                    undo_engine_.perform(
                        std::move(operation),
                        origin,
                        [trackIndex, callback = std::move(callback)](
                            ProjectUndoResult result) mutable {if (!result.succeeded()) {
                                callback(-1, std::move(result.error));
                                return;
                            }
                            callback(trackIndex, {});
                        });
                };
                dispatchToModelThread(std::move(finish));
            });
    }

    std::shared_ptr<ProjectUndoableOperation> TimelineFacadeImpl::makeTrackStructureOperation(
            TrackStructureUndoOperation::InitialDirection initialDirection,
            int32_t insertionIndex,
            ProjectTrackFragment fragment) {
                return std::make_shared<TrackStructureUndoOperation>(
            initialDirection,
            insertionIndex,
            std::move(fragment),
            [this](std::string_view trackReferenceId) {
                const auto currentIndex = trackIndexForPersistentId(trackReferenceId);
                return currentIndex >= 0 && engine_.removeTrack(currentIndex);
            },
            [this](const ProjectTrackFragment& captured,
                   int32_t restoreIndex,
                   ProjectUndoCompletion completion) {
                ProjectTrackAttachOptions options;
                options.idPolicy = ProjectObjectIdPolicy::Restore;
                options.insertionIndex = restoreIndex;
                attachTrackFragment(
                    captured,
                    options,
                    [completion = std::move(completion)](
                        int32_t attachedIndex,
                        std::string error) mutable {
                            if (!completion)
                            return;
                        if (attachedIndex >= 0 && error.empty()) {
                            completion(ProjectUndoResult::success());
                            return;
                        }
                        completion(ProjectUndoResult::failure(
                            error.empty() ? "Could not restore the track" : std::move(error)));
                    });
            });
    }

    std::optional<ProjectClipFragment> TimelineFacadeImpl::captureClipFragment(
            int32_t trackIndex,
            int32_t clipId) const {
                // Extensions contribute state here, and at least one of them --
        // ARA -- cannot legally archive while the document is being edited.
        // Refusing at this boundary reports the mistake at the real call
        // site rather than as a missing slot discovered much later.
        if (project_document_events_.inTransaction()) {
            std::cerr << "Error: captureClipFragment must not be called inside a document "
                         "transaction; capture first, then mutate." << std::endl;
            return std::nullopt;
        }

        const auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return std::nullopt;
        const auto* clip = targetTrack->clipManager().getClip(clipId);
        if (!clip)
            return std::nullopt;

        ProjectClipFragment fragment;
        fragment.clip = *clip;
        if (clip->clipType == ClipType::Midi) {
            // Audio clips are rebuilt from their file, but MIDI content is
            // authored and exists nowhere else, so it must be copied out.
            auto sourceNode = const_cast<TimelineTrack*>(targetTrack)
                ->getSourceNode(clip->sourceNodeInstanceId);
            if (auto midi = std::dynamic_pointer_cast<MidiClipSourceNode>(sourceNode)) {
                fragment.umpEvents = midi->umpEvents();
                fragment.umpTickTimestamps = midi->eventTimestampsTicks();
                fragment.tempoChanges = midi->tempoChanges();
                fragment.timeSignatureChanges = midi->timeSignatureChanges();
            }
        }

        // Collect state owned outside the document, such as a plug-in's
        // opaque per-clip state, so the fragment is self-contained.
        for (auto* extension : projectSerializationExtensionsSnapshot()) {
            std::vector<uint8_t> state;
            std::string extensionError;
            if (!extension->captureClipFragmentState(clip->referenceId, state, extensionError)) {
                // Continuing would hand back a fragment that looks complete
                // but has lost state the user cannot see and cannot
                // recover. Failing the capture keeps the operation honest.
                std::cerr << "Error: Extension " << extension->extensionId()
                          << " could not capture its state for clip "
                          << clip->referenceId << ": " << extensionError
                          << ". Capture abandoned rather than returning an incomplete fragment."
                          << std::endl;
                return std::nullopt;
            }
            if (!state.empty())
                fragment.extensionState[std::string(extension->extensionId())] = std::move(state);
        }
        return fragment;
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::attachClipFragment(
            int32_t trackIndex,
            const ProjectClipFragment& fragment,
            ProjectObjectIdPolicy idPolicy) {
                ClipAddResult result;
        auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack) {
            result.error = "Invalid track index";
            return result;
        }

        // Recreating the clip and reapplying its metadata is one edit.
        ProjectDocumentTransaction transaction(project_document_events_);

        // Restore reuses the captured identity; Mint leaves the staging
        // slot empty so the usual allocation happens.
        pending_clip_reference_id_ = idPolicy == ProjectObjectIdPolicy::Restore
            ? fragment.clip.referenceId
            : std::string{};

        const auto& source = fragment.clip;
        if (fragment.isMidi()) {
            result = addMidiClipToTimelineTrack(
                *targetTrack,
                source.position,
                source.filepath,
                fragment.umpEvents,
                fragment.umpTickTimestamps,
                source.tickResolution,
                source.clipTempo,
                fragment.tempoChanges,
                fragment.timeSignatureChanges,
                source.name,
                source.nrpnToParameterMapping,
                source.needsFileSave,
                idPolicy == ProjectObjectIdPolicy::Restore ? source.clipId : -1);
        } else {std::unique_ptr<AudioFileReader> reader;
            if (source.filepath.empty()) {
                reader = std::make_unique<SilentAudioFileReader>(
                    static_cast<uint64_t>(std::max<int64_t>(1, source.durationSamples)),
                    std::max<uint32_t>(1, targetTrack->channelCount()),
                    static_cast<uint32_t>(std::max(1, sampleRate_)));
            } else {reader = createAudioFileReaderFromPath(source.filepath);
            }
            if (!reader) {
                pending_clip_reference_id_.clear();
                result.error = "Could not reopen the audio file for the clip";
                return result;
            }
            result = addAudioClipToTrack(
                *targetTrack,
                source.position,
                std::move(reader),
                source.filepath,
                source.markers,
                source.audioWarps,
                idPolicy == ProjectObjectIdPolicy::Restore ? source.clipId : -1);
        }

        // Staging is consumed by a successful add; clear it so a failed one
        // cannot hand the captured identity to an unrelated later clip.
        pending_clip_reference_id_.clear();
        if (!result.success)
            return result;

        // Properties the add paths do not take. Applied through the
        // mutators so each is a document change like any other; the
        // surrounding transaction keeps them one batch.
        auto& clips = targetTrack->clipManager();
        clips.setClipGain(result.clipId, source.gain);
        clips.setClipMuted(result.clipId, source.muted);
        if (!clips.setClipAnchor(result.clipId, source.timeReference(sampleRate_), sampleRate_)) {
            removeClipRaw(*targetTrack, result.clipId);
            result.success = false;
            result.error = "Could not restore the clip's timeline anchor";
            return result;
        }
        commands_.setClipEnabled(
            trackIndex, result.clipId, source.enabled,
            ProjectMutationOrigin::Internal);
        commands_.resizeClip(
            trackIndex, result.clipId, source.durationSamples,
            ProjectMutationOrigin::Internal);
        if (fragment.isMidi() && !source.markers.empty())
            commands_.setClipMarkers(
                trackIndex, result.clipId, source.markers,
                ProjectMutationOrigin::Internal);

        // Hand each extension its own slot back, addressed by the identity
        // the clip has now: the captured one when restoring, a freshly
        // minted one when pasting.
        if (const auto* attachedClip = clips.getClip(result.clipId)) {
            for (auto* extension : projectSerializationExtensionsSnapshot()) {
                const auto it = fragment.extensionState.find(std::string(extension->extensionId()));
                static const std::vector<uint8_t> kNoState{};
                const auto& state = it == fragment.extensionState.end() ? kNoState : it->second;
                std::string extensionError;
                if (!extension->restoreClipFragmentState(attachedClip->referenceId, state, extensionError)) {
                    const auto failedClipReferenceId = attachedClip->referenceId;
                    removeClipRaw(*targetTrack, result.clipId);
                    result.success = false;
                    result.error = std::format(
                        "Extension {} failed to restore state for clip {}: {}",
                        extension->extensionId(),
                        failedClipReferenceId,
                        extensionError);
                    return result;
                }
            }
        }
        return result;
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::recordAddedClip(
            int32_t trackIndex,
            ClipAddResult result,
            ProjectMutationOrigin origin) {
                if (!result.success
            || (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote))
            return result;

        auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack) {
            result.success = false;
            result.error = "The added clip's track no longer exists";
            return result;
        }

        // The persistent identity and extension-owned state do not exist
        // until construction succeeds. Capture immediately afterwards,
        // outside a document transaction, then register the already-
        // performed operation. If either step fails, remove the new clip
        // so an untracked user mutation never leaks into the document.
        auto fragment = captureClipFragment(trackIndex, result.clipId);
        if (!fragment) {
            removeClipRaw(*targetTrack, result.clipId);
            result.success = false;
            result.error = "Could not capture the added clip for undo history";
            return result;
        }

        auto operation = std::make_shared<ClipAdditionUndoOperation>(
            targetTrack->referenceId(),
            std::move(*fragment),
            [this](std::string_view persistentTrackId, std::string_view persistentClipId) {
                return removeClipByReferenceId(persistentTrackId, persistentClipId);
            },
            [this](std::string_view persistentTrackId, const ProjectClipFragment& captured) {
                return restoreClipByReferenceId(persistentTrackId, captured);
            });
        auto recorded = std::make_shared<std::optional<ProjectUndoResult>>();
        undo_engine_.recordPerformed(
            std::move(operation),
            origin,
            [recorded](ProjectUndoResult completed) {
                *recorded = std::move(completed);
            });
        if (recorded->has_value() && recorded->value().succeeded())
            return result;

        removeClipRaw(*targetTrack, result.clipId);
        result.success = false;
        result.error = recorded->has_value() && !recorded->value().error.empty()
            ? recorded->value().error
            : "Could not record the added clip in undo history";
        return result;
    }

    bool TimelineFacadeImpl::recordReplacedClip(
            int32_t trackIndex,
            ProjectClipFragment before,
            ProjectMutationOrigin origin,
            std::string description) {
                auto* targetTrack = resolveTrack(trackIndex);
        if (!targetTrack)
            return false;
        auto after = captureClipFragment(trackIndex, before.clip.clipId);
        if (!after) {
            replaceClipByReferenceId(targetTrack->referenceId(), before, nullptr);
            return false;
        }
        auto operation = std::make_shared<ClipContentUndoOperation>(
            std::move(description),
            targetTrack->referenceId(),
            before,
            *after,
            [this](std::string_view persistentTrackId,
                   const ProjectClipFragment& desired,
                   const ProjectClipFragment& compensation) {
                return replaceClipByReferenceId(
                    persistentTrackId, desired, &compensation);
            });
        auto recorded = std::make_shared<std::optional<ProjectUndoResult>>();
        undo_engine_.recordPerformed(
            std::move(operation),
            origin,
            [recorded](ProjectUndoResult result) {
                *recorded = std::move(result);
            });
        if (recorded->has_value() && recorded->value().succeeded())
            return true;
        replaceClipByReferenceId(targetTrack->referenceId(), before, &*after);
        return false;
    }

    bool TimelineFacadeImpl::performCapturedClipRemoval(
            std::string trackReferenceId,
            ProjectClipFragment fragment,
            ProjectMutationOrigin origin) {
                if (origin != ProjectMutationOrigin::User && origin != ProjectMutationOrigin::Remote)
            return removeClipByReferenceId(trackReferenceId, fragment.clip.referenceId);

        auto operation = std::make_shared<ClipRemovalUndoOperation>(
            std::move(trackReferenceId),
            std::move(fragment),
            [this](std::string_view persistentTrackId, std::string_view persistentClipId) {
                return removeClipByReferenceId(persistentTrackId, persistentClipId);
            },
            [this](std::string_view persistentTrackId, const ProjectClipFragment& captured) {
                return restoreClipByReferenceId(persistentTrackId, captured);
            });
        auto result = std::make_shared<std::optional<ProjectUndoResult>>();
        undo_engine_.perform(
            std::move(operation),
            origin,
            [result](ProjectUndoResult completed) {
                *result = std::move(completed);
            });
        return result->has_value() && result->value().succeeded();
    }

    bool TimelineFacadeImpl::removeClipByReferenceId(
            std::string_view trackReferenceId,
            std::string_view clipReferenceId) {
                auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
        if (!targetTrack)
            return false;
        const auto clipId = clipIdForReferenceId(*targetTrack, clipReferenceId);
        return clipId >= 0 && removeClipRaw(*targetTrack, clipId);
    }

    TimelineFacade::ClipAddResult TimelineFacadeImpl::restoreClipByReferenceId(
            std::string_view trackReferenceId,
            const ProjectClipFragment& fragment) {
                const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
        if (trackIndex < 0 && trackIndex != kMasterTrackIndex)
            return {.error = "The clip's track no longer exists"};
        return attachClipFragment(trackIndex, fragment, ProjectObjectIdPolicy::Restore);
    }

    bool TimelineFacadeImpl::replaceClipByReferenceId(
            std::string_view trackReferenceId,
            const ProjectClipFragment& desired,
            const ProjectClipFragment* compensation) {
                auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
        if (!targetTrack)
            return false;
        const auto currentClipId =
            clipIdForReferenceId(*targetTrack, desired.clip.referenceId);
        if (currentClipId < 0 || !removeClipRaw(*targetTrack, currentClipId))
            return false;
        auto restored = restoreClipByReferenceId(trackReferenceId, desired);
        if (restored.success) {
            resolveAllClipAnchors();
            return true;
        }
        if (compensation) {
            const auto compensated =
                restoreClipByReferenceId(trackReferenceId, *compensation);
            if (compensated.success)
                resolveAllClipAnchors();
        }
        return false;
    }

    bool TimelineFacadeImpl::removeClipRaw(TimelineTrack& targetTrack, int32_t clipId) {
        const auto* clip = targetTrack.clipManager().getClip(clipId);
        if (!clip)
            return false;
        auto removedClip = *clip;
        if (!targetTrack.removeClip(clipId))
            return false;
        applyAuthoritativeTempoMapToMusicalClips();
        emitClipRemoved(targetTrack, removedClip);
        if (removedClip.clipType == ClipType::Midi)
            emitMasterTrackChanged("master-track-content-changed");
        notifyTimelineChanged();
        return true;
    }

} // namespace uapmd
