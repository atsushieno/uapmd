#include "TimelineFacadeImpl.hpp"

// Project loading and saving, including the asynchronous staging of plug-in
// state and graph serialization.

namespace uapmd {
    void TimelineFacadeImpl::saveProject(
            const std::filesystem::path& projectFile,
            ProjectSaveOptions options,
            ProjectSaveCallback callback) {
                auto operation = std::make_shared<PendingProjectSaveContext>();
        operation->project_file = projectFile;
        operation->emit_document_event = options.emitDocumentEvent;
        operation->mark_history_saved = options.markHistorySaved;
        operation->history_state_id = undo_engine_.state().currentStateId;
        operation->callback = std::move(callback);

        auto complete = [operation](ProjectResult result) mutable {
            if (!operation->callback)
                return;
            auto callback = std::move(operation->callback);
            callback(std::move(result));
        };

        if (engine_.frozenTrackManager().hasBusyTrack()) {
            complete(ProjectResult{
                false,
                "Unfreeze the busy track before saving the project"});
            return;
        }
        if (undo_engine_.state().busy) {
            complete(ProjectResult{
                false,
                "Wait for the pending undo operation before saving the project"});
            return;
        }
        if (projectFile.empty()) {
            complete(ProjectResult{false, "Project path is empty"});
            return;
        }

        try {
            operation->project_dir = projectFile.parent_path();
            if (!operation->project_dir.empty())
                std::filesystem::create_directories(operation->project_dir);
            auto clipDir = operation->project_dir / "clips";
            operation->plugin_state_dir = operation->project_dir / "plugin_states";
            operation->graph_dir = operation->project_dir / "graphs";
            operation->project = UapmdProjectData::create();
            std::unordered_set<int32_t> excludedTrackIndexes(
                options.excludedTrackIndexes.begin(),
                options.excludedTrackIndexes.end());

            struct SerializedTrackClips {int32_t trackIndex{
                    0};
                std::vector<ClipData> clips;
            };
            std::unordered_map<std::string, UapmdProjectClipData*> serializedClipLookup;
            std::vector<SerializedTrackClips> serializedTracks;
            size_t midiExportCounter = 0;

            auto sequencerTracks = engine_.tracks();
            auto timelineTracks = tracks();
            for (size_t trackIndex = 0; trackIndex < timelineTracks.size(); ++trackIndex) {
                if (excludedTrackIndexes.contains(static_cast<int32_t>(trackIndex)))
                    continue;

                auto* timelineTrack = timelineTracks[trackIndex];
                if (!timelineTrack)
                    continue;

                auto projectTrack = UapmdProjectTrackData::create();
                projectTrack->referenceId(timelineTrack->referenceId());
                auto clips = sequencer_detail::sortedTrackClips(*timelineTrack);
                serializedTracks.push_back(SerializedTrackClips{
                    static_cast<int32_t>(trackIndex),
                    clips});

                for (const auto& clip : clips) {
                    std::string clipError;
                    if (!sequencer_detail::serializeProjectClip(
                            *timelineTrack,
                            clip,
                            *projectTrack,
                            serializedClipLookup,
                            clipDir,
                            operation->project_dir,
                            std::format("track{}_clip_", trackIndex),
                            std::format("track{}_", trackIndex),
                            std::format("Clip {} on track {}", clip.clipId, trackIndex),
                            false,
                            midiExportCounter,
                            clipError)) {
                        complete(ProjectResult{false, std::move(clipError)});
                        return;
                    }
                }

                SequencerTrack* sequencerTrack = trackIndex < sequencerTracks.size()
                    ? sequencerTracks[trackIndex]
                    : nullptr;
                if (sequencerTrack)
                    projectTrack->volume(sequencerTrack->trackGain());
                if (sequencerTrack) {
                    projectTrack->muted(sequencerTrack->muted());
                    projectTrack->solo(sequencerTrack->solo());
                }
                bool hasClips = !projectTrack->clips().empty();
                bool hasPlugins = sequencerTrack && !sequencerTrack->orderedInstanceIds().empty();
                bool hasMixerState = sequencerTrack &&
                    (sequencerTrack->trackGain() != 1.0 || sequencerTrack->muted() || sequencerTrack->solo());
                if (!hasClips && !hasPlugins && !hasMixerState)
                    continue;

                queueProjectGraphSerialization(
                    *operation,
                    sequencerTrack,
                    *projectTrack,
                    std::format("track{}", trackIndex));

                operation->project->addTrack(std::move(projectTrack));
            }

            if (auto* masterTrack = operation->project->masterTrack()) {
                masterTrack->clips().clear();
                masterTrack->markers(engine_.masterTrackMarkers());
                if (engine_.masterTrack())
                    masterTrack->volume(engine_.masterTrack()->trackGain());
                if (master_timeline_track_) {
                    auto clips = sequencer_detail::sortedTrackClips(*master_timeline_track_);
                    serializedTracks.push_back(SerializedTrackClips{
                        kMasterTrackIndex,
                        clips});

                    for (const auto& clip : clips) {
                        if (clip.clipType != ClipType::Midi)
                            continue;

                        std::string clipError;
                        if (!sequencer_detail::serializeProjectClip(
                                *master_timeline_track_,
                                clip,
                                *masterTrack,
                                serializedClipLookup,
                                clipDir,
                                operation->project_dir,
                                "master_clip_",
                                "",
                                "Master clip",
                                true,
                                midiExportCounter,
                                clipError)) {
                            complete(ProjectResult{false, std::move(clipError)});
                            return;
                        }
                    }
                }

                queueProjectGraphSerialization(
                    *operation,
                    engine_.masterTrack(),
                    *masterTrack,
                    "master");
            }

            for (const auto& serializedTrack : serializedTracks) {
                for (const auto& clip : serializedTrack.clips) {
                    auto clipIt = serializedClipLookup.find(clip.referenceId);
                    if (clipIt == serializedClipLookup.end())
                        continue;

                    const auto timeReference = clip.timeReference(sampleRate_);
                    UapmdTimelinePosition pos{};
                    if (!timeReference.referenceId.empty()) {
                        auto anchorIt = serializedClipLookup.find(timeReference.referenceId);
                        if (anchorIt != serializedClipLookup.end())
                            pos.anchor = anchorIt->second;
                    }
                    pos.origin = (timeReference.type == TimeReferenceType::ContainerEnd)
                        ? UapmdAnchorOrigin::End
                        : UapmdAnchorOrigin::Start;
                    pos.samples = static_cast<uint64_t>(std::max<int64_t>(0,
                        TimelinePosition::fromSeconds(timeReference.offset, sampleRate_).samples));
                    clipIt->second->position(pos);
                }
            }
        } catch (const std::exception& e) {
            complete(ProjectResult{false, e.what()});
            return;
        }

        auto runNext = std::make_shared<std::function<void()>>();
        *runNext = [this, operation, complete, runNext]() mutable {if (operation->next_pending_state >= operation->pending_states.size()) {
                for (const auto& pendingGraph : operation->pending_graphs) {
                    if (!pendingGraph.track || !pendingGraph.sequencer_track || !pendingGraph.track->graph())
                        continue;

                    std::string graphWriteError;
                    if (!saveProjectGraph(
                            pendingGraph.track,
                            pendingGraph.sequencer_track,
                            operation->project_dir,
                            operation->graph_dir,
                            pendingGraph.scope_label,
                            graphWriteError)) {
                        complete(ProjectResult{false, std::move(graphWriteError)});
                        return;
                    }
                }
                std::string projectDataError;
                if (!saveProjectDataExtensions(*operation->project, projectDataError)) {
                    complete(ProjectResult{false, std::move(projectDataError)});
                    return;
                }
                if (!UapmdProjectDataWriter::write(operation->project.get(), operation->project_file)) {
                    complete(ProjectResult{false, "Failed to write project file"});
                    return;
                }
                std::string extensionError;
                if (!saveProjectExtensionData(
                        operation->project_file,
                        operation->project_dir,
                        extensionError)) {
                    complete(ProjectResult{false, std::move(extensionError)});
                    return;
                }
                auto finishSuccessfulSave = [this, operation, complete]() mutable {
                    if (operation->mark_history_saved)
                        undo_engine_.markStateSaved(operation->history_state_id);
                    if (operation->emit_document_event) {
                        ProjectDocumentEvent savedEvent(ProjectDocumentEventKind::ProjectSaved, "project-saved");
                        savedEvent.setProjectId(operation->project_file.string())
                            .setDetail("source.file", operation->project_file.string());
                        emitProjectDocumentEvent(std::move(savedEvent));
                    }
                    complete(ProjectResult{true, {}});
                };
                if (remidy::EventLoop::runningOnMainThread())
                    finishSuccessfulSave();
                else
                    remidy::EventLoop::enqueueTaskOnMainThread(std::move(finishSuccessfulSave));
                return;
            }

            auto pending = operation->pending_states[operation->next_pending_state];
            if (!pending.instance) {
                ++operation->next_pending_state;
                (*runNext)();
                return;
            }

            pending.instance->requestState(StateContextType::Project, false, nullptr,
                                           [operation, complete, runNext, pending](std::vector<uint8_t> state, std::string error, void* callbackContext) mutable {
                                               (void) callbackContext;
                                               if (!error.empty()) {
                                                   complete(TimelineFacade::ProjectResult{false, std::format("Failed to retrieve plugin state for instance {}: {}",
                                                                                                              pending.instance_id, error)});
                                                   return;
                                               }

                                               std::string writeError;
                                               auto relativePath = sequencer_detail::writePluginStateBlob(operation->project_dir,
                                                                                                          operation->plugin_state_dir,
                                                                                                          pending.scope_label,
                                                                                                          pending.plugin_order,
                                                                                                          pending.instance_id,
                                                                                                          state,
                                                                                                          writeError);
                                               if (!writeError.empty()) {
                                                   complete(TimelineFacade::ProjectResult{false, std::move(writeError)});
                                                   return;
                                               }

                                               if (pending.set_state_file)
                                                   pending.set_state_file(relativePath);

                                               ++operation->next_pending_state;
                                               (*runNext)();
                                           });
        };
        (*runNext)();
    }

    void TimelineFacadeImpl::loadProject(const std::filesystem::path& projectFile, ProjectLoadCallback callback) {        if (engine_.frozenTrackManager().hasBusyTrack()) {
            callback({
                false,
                "Unfreeze the busy track before loading a project"});
            return;
        }
        if (undo_engine_.state().busy) {
            callback({false, "Wait for the pending undo operation before loading a project"});
            return;
        }
        if (projectFile.empty()) {
            callback({false, "Project path is empty"});
            return;
        }

        auto project = UapmdProjectDataReader::read(projectFile);
        if (!project) {
            callback({false, "Failed to parse project file"});
            return;
        }

        auto projectDir = projectFile.parent_path();

        // Parsing succeeded, so everything below replaces the current
        // project. Never replay old operations against the new object set.
        // A failed load leaves the partial replacement dirty; successful
        // completion establishes a clean history root below.
        undo_engine_.clear(false);

        ProjectDocumentEvent closingEvent(ProjectDocumentEventKind::ProjectClosing, "project-closing");
        closingEvent.setProjectId(projectFile.string())
            .setFullResyncRecommended(true)
            .setDetail("source.file", projectFile.string());
        emitProjectDocumentEvent(std::move(closingEvent));

        suppress_timeline_notification_ = true;
        suppress_project_document_events_ = true;

        timeline_.isPlaying = false;
        timeline_.playheadPosition = TimelinePosition{};
        timeline_.loopEnabled = false;
        timeline_.projectTickResolution = 0;
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

        // Sentinel starts at 1 (representing the synchronous setup phase).
        // Each addPluginToTrack call increments it; each callback decrements it.
        // Releasing the sentinel at the end of setup triggers finish when no plugins are pending.
        auto pending_plugins = std::make_shared<std::atomic<int>>(1);
        auto finish_holder = std::make_shared<std::function<void()>>();
        using PluginLoadStep = std::function<void(std::function<void()>)>;
        auto plugin_load_steps = std::make_shared<std::vector<PluginLoadStep>>();

        struct LoadedClipRef {TimelineTrack* track{
                nullptr};
            int32_t clipId{-1};
            std::string clipReferenceId;
        };
        std::unordered_map<UapmdProjectClipData*, LoadedClipRef> loadedClipRefs;

        // FIXME: we might have to reconsider how we adapt plugin instances instantiated here to the graph.
        //  Currently, `UapmdPluginGraphBuilder::build()` is practically no-op, but the project loads.
        //  It is because it is already added in a linear manner.
        //  But that may not be the right thing depending on the graphs (such as, full DAG).
        auto loadPluginsForTrack = [this, &projectDir, plugin_load_steps](UapmdProjectTrackData* projectTrack, int32_t trackIndex) {
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
                    } else {projectTrack->graph(std::move(loadedGraphData));
                        graphData = projectTrack->graph();
                    }
                }
            }
            if (!provider)
                return;
            auto* pluginHost = engine_.pluginHost();
            std::vector<remidy::PluginCatalogEntry> catalogEntries;
            bool catalogLoaded = false;
            auto ensureCatalogLoaded = [&]() -> std::vector<remidy::PluginCatalogEntry>& {if (!catalogLoaded && pluginHost) {
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
                // Restore the node under the identity it was saved with, so
                // that ARA archives and saved graph connections reconnect.
                std::string nodeId = plugin.node_id;
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

                plugin_load_steps->push_back(
                    [this, trackIndex, format = std::move(format), pluginId = std::move(pluginId),
                     resolvedState, groupIndex, pluginLabel, nodeId = std::move(nodeId)](std::function<void()> done) mutable {
                         engine_.addPluginToTrack(trackIndex, format, pluginId,
                            [this, resolvedState, groupIndex, pluginLabel, pluginId, format, done = std::move(done)](int32_t instanceId, int32_t, std::string error) mutable {auto finishPlugin = [done]() mutable {
                            if (done)
                                done();
                        };

                        if (!error.empty()) {
                            std::cerr << "Warning: Failed to instantiate plugin " << pluginLabel
                                      << " (" << format << ", ID=" << pluginId << "): " << error << std::endl;
                        } else if (instanceId >= 0) {
                            // Restore saved group assignment (overrides auto-assigned group).
                            if (groupIndex >= 0 && groupIndex <= 15)
                                engine_.setInstanceGroup(instanceId, static_cast<uint8_t>(groupIndex));

                            if (!resolvedState.empty()) {
                                auto* instance = engine_.getPluginInstance(instanceId);
                                if (instance) {
                                    std::ifstream f(resolvedState, std::ios::binary);
                                    if (f) {
                                        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
                                        plugin_state_mutation_depth_.fetch_add(
                                            1,
                                            std::memory_order_acq_rel);
                                        instance->loadStateSync(data);
                                        plugin_state_mutation_depth_.fetch_sub(
                                            1,
                                            std::memory_order_acq_rel);
                                    } else {std::cerr << "Warning: Failed to open state file for plugin "
                                                  << pluginLabel << ": " << resolvedState << std::endl;
                                    }
                                } else {std::cerr << "Warning: Failed to get plugin instance " << instanceId
                                              << " while restoring state for " << pluginLabel << std::endl;
                                }
                            }
                        }
                        finishPlugin();
                    },
                    std::move(nodeId));
                });
            }
        };

        auto* masterProjectTrack = project->masterTrack();
        std::vector<ClipMarker> masterTrackMarkers;
        if (masterProjectTrack)
            masterTrackMarkers = masterProjectTrack->markers();
        const bool hasExplicitMasterTrackClips = masterProjectTrack && !masterProjectTrack->clips().empty();

        std::string earlyError;
        auto& tracks = project->tracks();

        for (size_t i = 0; i < tracks.size() && earlyError.empty(); ++i) {
            // Restore the track under the identity it was saved with, so
            // that anything keyed by it (ARA persistent IDs, frozen track
            // state) still resolves after a reload.
            pending_track_reference_id_ = tracks[i]->referenceId();
            int32_t trackIndex = engine_.addEmptyTrack();
            pending_track_reference_id_.clear();
            if (trackIndex < 0) {
                earlyError = "Failed to create track";
                break;
            }

            loadPluginsForTrack(tracks[i], trackIndex);

            for (auto& clip : tracks[i]->clips()) {
                if (!clip)
                    continue;

                // Staged per clip, so a clip that fails to load cannot
                // leak its identity onto the next one.
                pending_clip_reference_id_ = clip->referenceId();

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
                        earlyError = "MIDI clip is missing file path";
                        break;
                    }
                    auto clipInfo = MidiClipReader::readAnyFormat(resolvedPath);
                    if (!clipInfo.success) {
                        earlyError = clipInfo.error.empty() ? "Failed to parse MIDI clip" : clipInfo.error;
                        break;
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
                            separated.hasMasterTrackClip(),
                            ProjectMutationOrigin::Load);
                        if (!loadResult.success) {
                            earlyError = loadResult.error.empty() ? "Failed to load MIDI clip" : loadResult.error;
                            break;
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
                            "",
                            ProjectMutationOrigin::Load);
                        if (!masterLoadResult.success) {
                            earlyError = masterLoadResult.error.empty() ? "Failed to load master track clip" : masterLoadResult.error;
                            break;
                        }
                        if (auto refIt = loadedClipRefs.find(clip.get()); refIt != loadedClipRefs.end() && !refIt->second.clipReferenceId.empty())
                            master_timeline_track_->clipManager().setClipAnchor(
                                masterLoadResult.clipId,
                                TimeReference::fromContainerStart(refIt->second.clipReferenceId, 0.0),
                                sampleRate_);
                    }
                } else {std::unique_ptr<AudioFileReader> reader;
                    std::string filepath;
                    if (resolvedPath.empty()) {
                        const int64_t durationSamples = std::max<int64_t>(
                            1,
                            clip->durationSamples() > 0
                                ? clip->durationSamples()
                                : static_cast<int64_t>(sampleRate_));
                        const uint32_t channelCount = std::max<uint32_t>(
                            1,
                            timeline_tracks_[static_cast<size_t>(trackIndex)]->channelCount());
                        reader = std::make_unique<SilentAudioFileReader>(
                            static_cast<uint64_t>(durationSamples),
                            channelCount,
                            static_cast<uint32_t>(sampleRate_));
                    } else {reader = createAudioFileReaderFromPath(resolvedPath.string());
                        if (!reader) {
                            earlyError = std::format("Failed to open audio clip {}", resolvedPath.string());
                            break;
                        }
                        filepath = resolvedPath.string();
                    }
                    auto loadResult = addAudioClipToTrack(
                        *timeline_tracks_[static_cast<size_t>(trackIndex)],
                        position,
                        std::move(reader),
                        filepath,
                        clip->markers(),
                        clip->audioWarps());
                    if (!loadResult.success) {
                        earlyError = loadResult.error.empty() ? "Failed to load audio clip" : loadResult.error;
                        break;
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
        if (earlyError.empty() && masterProjectTrack) {
            loadPluginsForTrack(masterProjectTrack, kMasterTrackIndex);
            for (auto& clip : masterProjectTrack->clips()) {
                if (!clip || clip->clipType() != "midi")
                    continue;
                pending_clip_reference_id_ = clip->referenceId();
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
                    resolvedPath.string(),
                    ProjectMutationOrigin::Load);
                if (!masterLoadResult.success) {
                    earlyError = masterLoadResult.error.empty() ? "Failed to load master track clip" : masterLoadResult.error;
                    break;
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

        if (earlyError.empty()) {
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
        }

        // Set finish_holder before releasing the sentinel.
        // finish_holder is always set before any plugin callback can observe pending==0.
        if (!earlyError.empty()) {
            suppress_timeline_notification_ = false;
            suppress_project_document_events_ = false;
            *finish_holder = [callback = std::move(callback), earlyError = std::move(earlyError)]() mutable {callback({
                    false, std::move(earlyError)});
            };
        } else {auto sharedProject = std::shared_ptr<UapmdProjectData>(std::move(project));
            auto finishLoadedProject = [this, projectFile, projectDir,
                                        sharedProject = std::move(sharedProject),
                                        masterTrackMarkers = std::move(masterTrackMarkers),
                                        callback = std::move(callback)]() mutable {
                                            suppress_timeline_notification_ = false;
                suppress_project_document_events_ = false;

                auto applyGraphConnections = [this](UapmdProjectTrackData* projectTrack, SequencerTrack* sequencerTrack) {
                    if (!projectTrack || !sequencerTrack || !projectTrack->graph())
                        return;
                    materializeProjectGraph(projectTrack, sequencerTrack, engine_.umpBufferSizeInBytes());
                };

                auto& tracks = sharedProject->tracks();
                auto* masterProjectTrack = sharedProject->masterTrack();
                for (size_t i = 0; i < tracks.size() && i < engine_.tracks().size(); ++i)
                    applyGraphConnections(tracks[i], engine_.tracks()[i]);
                if (masterProjectTrack)
                    applyGraphConnections(masterProjectTrack, engine_.masterTrack());

                std::string projectDataError;
                if (!loadProjectDataExtensions(*sharedProject, projectDataError)) {
                    callback({false, std::move(projectDataError)});
                    return;
                }

                for (size_t i = 0; i < tracks.size() && i < engine_.tracks().size(); ++i) {
                    if (tracks[i] && engine_.tracks()[i]) {
                        engine_.tracks()[i]->trackGain(tracks[i]->volume());
                        engine_.tracks()[i]->muted(tracks[i]->muted());
                        engine_.tracks()[i]->solo(tracks[i]->solo());
                    }
                }
                if (masterProjectTrack && engine_.masterTrack()) {
                    engine_.masterTrack()->trackGain(masterProjectTrack->volume());
                }

                ProjectDocumentEvent loadedEvent(ProjectDocumentEventKind::ProjectLoaded, "project-loaded");
                loadedEvent.setProjectId(projectFile.string())
                    .setFullResyncRecommended(true)
                    .setDetail("source.file", projectFile.string());
                emitProjectDocumentEvent(std::move(loadedEvent));
                emitMasterTrackChanged("master-track-content-changed");
                std::string extensionError;
                if (!loadProjectExtensionData(projectFile, projectDir, extensionError)) {
                    callback({false, std::move(extensionError)});
                    return;
                }
                undo_engine_.clear(true);
                notifyTimelineChanged();
            engine_.setMasterTrackMarkers(std::move(masterTrackMarkers));
            callback({true, {}});
            };
            *finish_holder = [finish = std::move(finishLoadedProject)]() mutable {
                if (remidy::EventLoop::runningOnMainThread())
                    finish();
                else
                    remidy::EventLoop::enqueueTaskOnMainThread(std::move(finish));
            };
        }

        if (earlyError.empty() && !plugin_load_steps->empty()) {
            pending_plugins->fetch_add(1, std::memory_order_relaxed);
            auto next_index = std::make_shared<size_t>(0);
            auto run_next = std::make_shared<std::function<void()>>();
            *run_next = [plugin_load_steps, next_index, run_next, pending_plugins, finish_holder]() mutable {if (*next_index >= plugin_load_steps->size()) {
                    if (pending_plugins->fetch_sub(1, std::memory_order_acq_rel) == 1)
                        (*finish_holder)();
                    return;
                }
                auto step = (*plugin_load_steps)[(*next_index)++];
                step([run_next]() mutable {
                    (*run_next)();
                });
            };
            (*run_next)();
        }

        // Release the sentinel; if it reaches 0 (no plugins pending), fire finish now.
        if (pending_plugins->fetch_sub(1, std::memory_order_acq_rel) == 1)
            (*finish_holder)();
    }

    void TimelineFacadeImpl::queueProjectGraphSerialization(
            PendingProjectSaveContext& operation,
            SequencerTrack* sequencerTrack,
            UapmdProjectTrackData& projectTrack,
            const std::string& scopeLabel) {
                const auto* graphProvider = sequencerTrack
            ? audio_graph_provider_registry_.get(sequencerTrack->graph())
            : audio_graph_provider_registry_.get("");
        if (!sequencerTrack || !graphProvider)
            return;

        auto graphData = createSerializedProjectGraph(
            *graphProvider,
            sequencerTrack->orderedInstanceIds(),
            sequencerTrack->graph(),
            [this](int32_t instanceId) {
                return engine_.getPluginInstance(instanceId);
            },
            [&operation, &scopeLabel](int32_t instanceId, size_t pluginOrder, AudioPluginInstanceAPI* instance,
                                      const std::function<void(const std::string& relativePath)>& setStateFile) {
                if (setStateFile)
                    setStateFile({});
                operation.pending_states.push_back(PendingProjectPluginState{
                    .instance_id = instanceId,
                    .plugin_order = pluginOrder,
                    .instance = instance,
                    .set_state_file = setStateFile,
                    .scope_label = scopeLabel
                });
            });
        if (!graphData)
            return;

        projectTrack.graph(std::move(graphData));
        operation.pending_graphs.push_back(PendingProjectGraphSave{
            .track = &projectTrack,
            .sequencer_track = sequencerTrack,
            .scope_label = scopeLabel
        });
    }

    bool TimelineFacadeImpl::materializeProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            size_t eventBufferSizeInBytes) {
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
        if (!replaceTrackGraphType(
                trackIndex,
                provider->id(),
                eventBufferSizeInBytes,
                ProjectMutationOrigin::Internal))
            return false;
        return provider->deserializeRuntimeGraph(
            projectTrack->graph(), sequencerTrack->graph(), sequencerTrack->orderedInstanceIds());
    }

    bool TimelineFacadeImpl::saveProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            const std::filesystem::path& projectDir,
            const std::filesystem::path& graphDir,
            const std::string& scopeLabel,
            std::string& error) {
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

    bool TimelineFacadeImpl::saveProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            extensions = project_serialization_extensions_;
        }
        for (auto* extension : extensions) {
            if (!extension)
                continue;
            std::string extensionError;
            if (!extension->saveProjectData(project, extensionError)) {
                error = std::format("Failed to save project data for extension {}: {}",
                                    extension->extensionId(), extensionError);
                return false;
            }
        }
        return true;
    }

    bool TimelineFacadeImpl::loadProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            extensions = project_serialization_extensions_;
        }
        for (auto* extension : extensions) {
            if (!extension)
                continue;
            std::string extensionError;
            if (!extension->loadProjectData(project, extensionError)) {
                error = std::format("Failed to load project data for extension {}: {}",
                                    extension->extensionId(), extensionError);
                return false;
            }
        }
        return true;
    }

    bool TimelineFacadeImpl::saveProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            extensions = project_serialization_extensions_;
        }

        sequencer_detail::FilesystemProjectSerializationWriteContext context(projectFile, projectDir);
        for (auto* extension : extensions) {
            if (!extension)
                continue;
            std::string extensionError;
            if (!extension->saveProjectExtensionData(context, extensionError)) {
                error = std::format("Failed to save project extension {}: {}",
                                    extension->extensionId(),
                                    extensionError);
                return false;
            }
        }
        return true;
    }

    bool TimelineFacadeImpl::loadProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            extensions = project_serialization_extensions_;
        }

        sequencer_detail::FilesystemProjectSerializationReadContext context(projectFile, projectDir);
        for (auto* extension : extensions) {
            if (!extension)
                continue;
            std::string extensionError;
            if (!extension->loadProjectExtensionData(context, extensionError)) {
                error = std::format("Failed to load project extension {}: {}",
                                    extension->extensionId(),
                                    extensionError);
                return false;
            }
        }
        return true;
    }

    bool TimelineFacadeImpl::writeBinaryFile(
            const std::filesystem::path& path,
            const std::vector<uint8_t>& bytes,
            std::string& error) {
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

} // namespace uapmd
