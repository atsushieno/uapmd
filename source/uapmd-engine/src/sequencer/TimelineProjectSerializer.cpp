#include "TimelineProjectSerializer.hpp"

#include <charconv>
#include <unordered_set>
#include <fstream>
#include <iostream>

#include "ProjectSerialization.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

// Project loading and saving: staging asynchronous plug-in state and graph
// serialization on the way out, and recreating the document on the way in.

using namespace uapmd_plugin_hosting;

namespace uapmd::timeline_detail {

    bool writeBinaryFile(
        const std::filesystem::path& path,
        const std::vector<uint8_t>& bytes,
        std::string& error);

    namespace {
    std::filesystem::path makeRelativePath(
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

    std::filesystem::path makeAbsolutePath(
            const std::filesystem::path& baseDir,
            const std::filesystem::path& target)
        {
            if (target.empty())
                return target;
            if (target.is_absolute() || baseDir.empty())
                return std::filesystem::absolute(target);
            return std::filesystem::absolute(baseDir / target);
        }

    std::string urlEscapeFilenameComponent(std::string_view value) {
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
    }


    // The clips written so far during one save, keyed by reference id, so that
    // a clip anchored to another can be linked once both exist in the document.
    struct ProjectSaveBuild {
        struct SerializedTrackClips {
            int32_t trackIndex{0};
            std::vector<ClipData> clips;
        };

        std::filesystem::path clipDir;
        std::unordered_map<std::string, UapmdProjectClipData*> clipLookup;
        std::vector<SerializedTrackClips> tracks;
        size_t midiExportCounter{0};
    };

    void TimelineProjectSerializer::saveProject(
        const std::filesystem::path& projectFile,
        TimelineFacade::ProjectSaveOptions options,
        TimelineFacade::ProjectSaveCallback callback) {
        auto operation = std::make_shared<PendingProjectSaveContext>();
        operation->project_file = projectFile;
        operation->emit_document_event = options.emitDocumentEvent;
        operation->mark_history_saved = options.markHistorySaved;
        operation->history_state_id = facade_.commands().history().state().currentStateId;
        operation->callback = std::move(callback);

        // Reports exactly once, from whichever asynchronous branch finishes
        // first. Plug-in state capture has several failure exits.
        ProjectSaveCompletion complete = [operation](TimelineFacade::ProjectResult result) mutable {
            if (!operation->callback)
                return;
            auto callback = std::move(operation->callback);
            callback(std::move(result));
        };

        std::string error;
        if (!beginProjectSave(*operation, error)) {
            complete({false, std::move(error)});
            return;
        }
        if (!buildProjectDocument(*operation, options, error)) {
            complete({false, std::move(error)});
            return;
        }
        // Everything remaining depends on plug-in state, which arrives
        // asynchronously.
        runPendingPluginStateCaptures(operation, std::move(complete));
    }

    // Rejects a save that cannot start. Nothing has been written at this point.
    bool TimelineProjectSerializer::beginProjectSave(
        PendingProjectSaveContext& operation,
        std::string& error) {
        if (engine_.frozenTrackManager().hasBusyTrack()) {
            error = "Unfreeze the busy track before saving the project";
            return false;
        }
        if (facade_.commands().history().state().busy) {
            error = "Wait for the pending undo operation before saving the project";
            return false;
        }
        if (operation.project_file.empty()) {
            error = "Project path is empty";
            return false;
        }
        return true;
    }

    // Builds the whole in-memory project document, and queues the plug-in
    // state and graph writes that can only run asynchronously. Filesystem
    // work here can throw, which is reported as an ordinary failure.
    bool TimelineProjectSerializer::buildProjectDocument(
        PendingProjectSaveContext& operation,
        const TimelineFacade::ProjectSaveOptions& options,
        std::string& error) {
        try {
            operation.project_dir = operation.project_file.parent_path();
            if (!operation.project_dir.empty())
                std::filesystem::create_directories(operation.project_dir);
            operation.plugin_state_dir = operation.project_dir / "plugin_states";
            operation.graph_dir = operation.project_dir / "graphs";
            operation.project = UapmdProjectData::create();

            ProjectSaveBuild build;
            build.clipDir = operation.project_dir / "clips";

            if (!serializeTracks(operation, options, build, error))
                return false;
            if (!serializeMasterTrack(operation, build, error))
                return false;
            applySerializedClipAnchors(build);
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    bool TimelineProjectSerializer::serializeTracks(
        PendingProjectSaveContext& operation,
        const TimelineFacade::ProjectSaveOptions& options,
        ProjectSaveBuild& build,
        std::string& error) {
        const std::unordered_set<int32_t> excludedTrackIndexes(
            options.excludedTrackIndexes.begin(),
            options.excludedTrackIndexes.end());

        auto sequencerTracks = engine_.tracks();
        auto timelineTracks = facade_.tracks();
        for (size_t trackIndex = 0; trackIndex < timelineTracks.size(); ++trackIndex) {
            if (excludedTrackIndexes.contains(static_cast<int32_t>(trackIndex)))
                continue;

            auto* timelineTrack = timelineTracks[trackIndex];
            if (!timelineTrack)
                continue;

            auto projectTrack = UapmdProjectTrackData::create();
            projectTrack->referenceId(timelineTrack->referenceId());
            auto clips = sequencer_detail::sortedTrackClips(*timelineTrack);
            build.tracks.push_back(ProjectSaveBuild::SerializedTrackClips{
                static_cast<int32_t>(trackIndex),
                clips});

            for (const auto& clip : clips) {
                if (!sequencer_detail::serializeProjectClip(
                        *timelineTrack,
                        clip,
                        *projectTrack,
                        build.clipLookup,
                        build.clipDir,
                        operation.project_dir,
                        std::format("track{}_clip_", trackIndex),
                        std::format("track{}_", trackIndex),
                        std::format("Clip {} on track {}", clip.clipId, trackIndex),
                        false,
                        build.midiExportCounter,
                        error))
                    return false;
            }

            SequencerTrack* sequencerTrack = trackIndex < sequencerTracks.size()
                ? sequencerTracks[trackIndex]
                : nullptr;
            if (sequencerTrack) {
                projectTrack->volume(sequencerTrack->trackGain());
                projectTrack->muted(sequencerTrack->muted());
                projectTrack->solo(sequencerTrack->solo());
            }

            // An empty track with default mixer state carries no information,
            // so it is not written at all.
            const bool hasClips = !projectTrack->clips().empty();
            const bool hasPlugins = sequencerTrack && !sequencerTrack->orderedInstanceIds().empty();
            const bool hasMixerState = sequencerTrack
                && (sequencerTrack->trackGain() != 1.0
                    || sequencerTrack->muted()
                    || sequencerTrack->solo());
            if (!hasClips && !hasPlugins && !hasMixerState)
                continue;

            queueProjectGraphSerialization(
                operation,
                sequencerTrack,
                *projectTrack,
                std::format("track{}", trackIndex));

            operation.project->addTrack(std::move(projectTrack));
        }
        return true;
    }

    // The master track carries the tempo/time-signature map and project-wide
    // markers. Only its MIDI clips are meaningful.
    bool TimelineProjectSerializer::serializeMasterTrack(
        PendingProjectSaveContext& operation,
        ProjectSaveBuild& build,
        std::string& error) {
        auto* masterTrack = operation.project->masterTrack();
        if (!masterTrack)
            return true;

        masterTrack->clips().clear();
        masterTrack->markers(engine_.masterTrackMarkers());
        if (engine_.masterTrack())
            masterTrack->volume(engine_.masterTrack()->trackGain());

        if (auto* masterTimelineTrack = facade_.masterTimelineTrack()) {
            auto clips = sequencer_detail::sortedTrackClips(*masterTimelineTrack);
            build.tracks.push_back(ProjectSaveBuild::SerializedTrackClips{
                kMasterTrackIndex,
                clips});

            for (const auto& clip : clips) {
                if (clip.clipType != ClipType::Midi)
                    continue;
                if (!sequencer_detail::serializeProjectClip(
                        *masterTimelineTrack,
                        clip,
                        *masterTrack,
                        build.clipLookup,
                        build.clipDir,
                        operation.project_dir,
                        "master_clip_",
                        "",
                        "Master clip",
                        true,
                        build.midiExportCounter,
                        error))
                    return false;
            }
        }

        queueProjectGraphSerialization(
            operation,
            engine_.masterTrack(),
            *masterTrack,
            "master");
        return true;
    }

    // A clip's position may be expressed relative to another clip, so anchors
    // are linked only once every clip has been written to the document.
    void TimelineProjectSerializer::applySerializedClipAnchors(ProjectSaveBuild& build) {
        for (const auto& serializedTrack : build.tracks) {
            for (const auto& clip : serializedTrack.clips) {
                auto clipIt = build.clipLookup.find(clip.referenceId);
                if (clipIt == build.clipLookup.end())
                    continue;

                const auto timeReference = clip.timeReference(host_.sampleRate());
                UapmdTimelinePosition pos{};
                if (!timeReference.referenceId.empty()) {
                    auto anchorIt = build.clipLookup.find(timeReference.referenceId);
                    if (anchorIt != build.clipLookup.end())
                        pos.anchor = anchorIt->second;
                }
                pos.origin = timeReference.type == TimeReferenceType::ContainerEnd
                    ? UapmdAnchorOrigin::End
                    : UapmdAnchorOrigin::Start;
                pos.samples = static_cast<uint64_t>(std::max<int64_t>(0,
                    TimelinePosition::fromSeconds(
                        timeReference.offset, host_.sampleRate()).samples));
                clipIt->second->position(pos);
            }
        }
    }

    // Walks the queued plug-ins one at a time, writing each one's opaque state
    // to its own file. Plug-ins report state asynchronously, so this drives
    // itself forward from each callback and hands over to the final write when
    // the queue is exhausted.
    void TimelineProjectSerializer::runPendingPluginStateCaptures(
        std::shared_ptr<PendingProjectSaveContext> operation,
        ProjectSaveCompletion complete) {
        auto runNext = std::make_shared<std::function<void()>>();
        *runNext = [this, operation, complete, runNext]() mutable {
            if (operation->next_pending_state >= operation->pending_states.size()) {
                writeSerializedProject(operation, complete);
                return;
            }

            auto pending = operation->pending_states[operation->next_pending_state];
            if (!pending.instance) {
                ++operation->next_pending_state;
                (*runNext)();
                return;
            }

            pending.instance->requestState(
                StateContextType::Project, false, nullptr,
                [operation, complete, runNext, pending](
                    std::vector<uint8_t> state, std::string error, void* callbackContext) mutable {
                    (void) callbackContext;
                    if (!error.empty()) {
                        complete({false, std::format(
                            "Failed to retrieve plugin state for instance {}: {}",
                            pending.instance_id, error)});
                        return;
                    }

                    std::string writeError;
                    auto relativePath = sequencer_detail::writePluginStateBlob(
                        operation->project_dir,
                        operation->plugin_state_dir,
                        pending.scope_label,
                        pending.plugin_order,
                        pending.instance_id,
                        state,
                        writeError);
                    if (!writeError.empty()) {
                        complete({false, std::move(writeError)});
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

    // Everything that can only happen once every plug-in has reported its
    // state: the graphs, the extension sidecars, and the project file itself.
    void TimelineProjectSerializer::writeSerializedProject(
        std::shared_ptr<PendingProjectSaveContext> operation,
        const ProjectSaveCompletion& complete) {
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
                complete({false, std::move(graphWriteError)});
                return;
            }
        }

        std::string projectDataError;
        if (!saveProjectDataExtensions(*operation->project, projectDataError)) {
            complete({false, std::move(projectDataError)});
            return;
        }
        if (!UapmdProjectDataWriter::write(operation->project.get(), operation->project_file)) {
            complete({false, "Failed to write project file"});
            return;
        }
        std::string extensionError;
        if (!saveProjectExtensionData(
                operation->project_file, operation->project_dir, extensionError)) {
            complete({false, std::move(extensionError)});
            return;
        }

        // Marking history and announcing the save touch the document, so they
        // run on the model thread even when the last plug-in reported from
        // another one.
        auto finish = [this, operation, complete]() mutable {
            finishSuccessfulSave(*operation, complete);
        };
        if (remidy::EventLoop::runningOnMainThread())
            finish();
        else
            remidy::EventLoop::enqueueTaskOnMainThread(std::move(finish));
    }

    void TimelineProjectSerializer::finishSuccessfulSave(
        PendingProjectSaveContext& operation,
        const ProjectSaveCompletion& complete) {
        if (operation.mark_history_saved)
            facade_.commands().history().markStateSaved(operation.history_state_id);
        if (operation.emit_document_event) {
            ProjectDocumentEvent savedEvent(
                ProjectDocumentEventKind::ProjectSaved, "project-saved");
            savedEvent.setProjectId(operation.project_file.string())
                .setDetail("source.file", operation.project_file.string());
            host_.emitProjectDocumentEvent(std::move(savedEvent));
        }
        complete({true, {}});
    }

    // A clip that was created during a load, together with the identity it
    // received, so that anchors expressed against another clip can be
    // resolved once every clip exists.
    struct LoadedClipRef {
        TimelineTrack* track{nullptr};
        int32_t clipId{-1};
        std::string clipReferenceId;
    };

    // One asynchronous plug-in instantiation, queued during setup and run in
    // order afterwards.
    using ProjectPluginLoadStep = std::function<void(std::function<void()>)>;

    // State shared by the phases of one project load.
    //
    // A load outlives its synchronous setup: plug-ins are instantiated through
    // callbacks, so the parts a callback touches are held by shared_ptr. The
    // sentinel in pendingPlugins starts at 1 for the setup phase itself, and
    // releasing it at the end fires completion when no plug-in is still
    // outstanding.
    struct ProjectLoadRun {
        std::filesystem::path file;
        std::filesystem::path dir;
        std::unique_ptr<UapmdProjectData> project;
        UapmdProjectTrackData* masterTrack{nullptr};
        std::vector<ClipMarker> masterTrackMarkers;
        bool hasExplicitMasterTrackClips{false};
        std::unordered_map<UapmdProjectClipData*, LoadedClipRef> loadedClips;

        std::shared_ptr<std::vector<ProjectPluginLoadStep>> pluginSteps{
            std::make_shared<std::vector<ProjectPluginLoadStep>>()};
        std::shared_ptr<std::atomic<int>> pendingPlugins{
            std::make_shared<std::atomic<int>>(1)};
        std::shared_ptr<std::function<void()>> finish{
            std::make_shared<std::function<void()>>()};

        std::string error;
        TimelineFacade::ProjectLoadCallback callback;
    };

    void TimelineProjectSerializer::loadProject(
        const std::filesystem::path& projectFile,
        TimelineFacade::ProjectLoadCallback callback) {
        ProjectLoadRun run;
        run.file = projectFile;
        run.callback = std::move(callback);

        if (!beginProjectLoad(run))
            return;
        resetDocumentForLoad(run);
        restoreProjectTracks(run);
        restoreMasterTrackClips(run);
        applyLoadedClipAnchors(run);
        installLoadCompletion(run);
        runQueuedPluginLoads(run);

        // Release the setup sentinel. If no plug-in is pending, completion
        // fires here rather than from a plug-in callback.
        if (run.pendingPlugins->fetch_sub(1, std::memory_order_acq_rel) == 1)
            (*run.finish)();
    }

    // Rejects a load that cannot start and parses the project file. Returns
    // false after invoking the callback, in which case the document has not
    // been touched.
    bool TimelineProjectSerializer::beginProjectLoad(ProjectLoadRun& run) {
        if (engine_.frozenTrackManager().hasBusyTrack()) {
            run.callback({false, "Unfreeze the busy track before loading a project"});
            return false;
        }
        if (facade_.commands().history().state().busy) {
            run.callback({false, "Wait for the pending undo operation before loading a project"});
            return false;
        }
        if (run.file.empty()) {
            run.callback({false, "Project path is empty"});
            return false;
        }

        run.project = UapmdProjectDataReader::read(run.file);
        if (!run.project) {
            run.callback({false, "Failed to parse project file"});
            return false;
        }
        run.dir = run.file.parent_path();
        run.masterTrack = run.project->masterTrack();
        if (run.masterTrack) {
            run.masterTrackMarkers = run.masterTrack->markers();
            run.hasExplicitMasterTrackClips = !run.masterTrack->clips().empty();
        }
        return true;
    }

    // Everything from here on replaces the current project, so history is
    // dropped first: old operations must never replay against the new object
    // set. A failed load leaves the partial replacement dirty; a successful
    // one establishes a clean history root at the end.
    void TimelineProjectSerializer::resetDocumentForLoad(ProjectLoadRun& run) {
        facade_.commands().history().clear(false);

        ProjectDocumentEvent closingEvent(
            ProjectDocumentEventKind::ProjectClosing, "project-closing");
        closingEvent.setProjectId(run.file.string())
            .setFullResyncRecommended(true)
            .setDetail("source.file", run.file.string());
        host_.emitProjectDocumentEvent(std::move(closingEvent));

        host_.setLoadInProgress(true);

        facade_.state().isPlaying = false;
        facade_.state().playheadPosition = TimelinePosition{};
        facade_.state().loopEnabled = false;
        facade_.state().projectTickResolution = 0;
        host_.resetTrackReferenceAllocator();
        host_.replaceMasterTimelineTrack(std::make_shared<TimelineTrack>(
            std::string("master_track"),
            0,
            host_.sampleRate() > 0 ? static_cast<double>(host_.sampleRate()) : 48000.0,
            host_.bufferSizeInFrames()));

        // Removal goes through the engine so that onTrackRemoved runs for each
        // track. SequencerEngine::tracks() returns a transient snapshot, so it
        // is refreshed every iteration.
        while (true) {
            auto& snapshot = engine_.tracks();
            if (snapshot.empty())
                break;
            engine_.removeTrack(static_cast<uapmd_track_index_t>(snapshot.size() - 1));
        }
    }

    // Queues one asynchronous instantiation per plug-in node on the track.
    // Nothing is instantiated yet: the steps run after the whole document
    // structure exists.
    //
    // FIXME: we might have to reconsider how we adapt plugin instances instantiated here to the graph.
    //  Currently, `UapmdPluginGraphBuilder::build()` is practically no-op, but the project loads.
    //  It is because it is already added in a linear manner.
    //  But that may not be the right thing depending on the graphs (such as, full DAG).
    void TimelineProjectSerializer::queuePluginLoadsForTrack(
        ProjectLoadRun& run,
        UapmdProjectTrackData* projectTrack,
        int32_t trackIndex) {
        if (!projectTrack)
            return;
        auto* graphData = projectTrack->graph();
        if (!graphData)
            return;
        auto* provider = facade_.audioGraphProviderRegistry().get(graphData->graphType());
        auto externalGraphFile = graphData->externalFile();
        if (!externalGraphFile.empty() && provider) {
            auto resolvedGraphFile = makeAbsolutePath(run.dir, externalGraphFile);
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
            // Restore the node under the identity it was saved with, so that
            // ARA archives and saved graph connections reconnect.
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
                resolvedState = makeAbsolutePath(run.dir, stateFile);

            const std::string pluginLabel = pluginName.empty() ? pluginId : pluginName;

            run.pluginSteps->push_back(
                [this, trackIndex, format = std::move(format), pluginId = std::move(pluginId),
                 resolvedState, groupIndex, pluginLabel, nodeId = std::move(nodeId)](
                    std::function<void()> done) mutable {
                    engine_.addPluginToTrack(
                        trackIndex, format, pluginId,
                        [this, resolvedState, groupIndex, pluginLabel, pluginId, format,
                         done = std::move(done)](
                            int32_t instanceId, int32_t, std::string error) mutable {
                            restoreLoadedPluginState(
                                instanceId, error, resolvedState, groupIndex, pluginLabel,
                                pluginId, format);
                            if (done)
                                done();
                        },
                        std::move(nodeId));
                });
        }
    }

    // Applies the saved group and opaque state to a plug-in that has just been
    // instantiated during a load. A plug-in that fails here is reported and
    // skipped: one unavailable plug-in must not abort the whole project.
    void TimelineProjectSerializer::restoreLoadedPluginState(
        int32_t instanceId,
        const std::string& instantiationError,
        const std::filesystem::path& stateFile,
        int32_t groupIndex,
        const std::string& pluginLabel,
        const std::string& pluginId,
        const std::string& format) {
        if (!instantiationError.empty()) {
            std::cerr << "Warning: Failed to instantiate plugin " << pluginLabel
                      << " (" << format << ", ID=" << pluginId << "): " << instantiationError << std::endl;
            return;
        }
        if (instanceId < 0)
            return;

        // A saved group assignment overrides the automatically assigned one.
        if (groupIndex >= 0 && groupIndex <= 15)
            engine_.setInstanceGroup(instanceId, static_cast<uint8_t>(groupIndex));
        if (stateFile.empty())
            return;

        auto* instance = engine_.getPluginInstance(instanceId);
        if (!instance) {
            std::cerr << "Warning: Failed to get plugin instance " << instanceId
                      << " while restoring state for " << pluginLabel << std::endl;
            return;
        }
        std::ifstream f(stateFile, std::ios::binary);
        if (!f) {
            std::cerr << "Warning: Failed to open state file for plugin "
                      << pluginLabel << ": " << stateFile << std::endl;
            return;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
        instance->loadStateSync(data);
    }

    void TimelineProjectSerializer::restoreProjectTracks(ProjectLoadRun& run) {
        auto& tracks = run.project->tracks();
        for (size_t i = 0; i < tracks.size() && run.error.empty(); ++i) {
            // Restore the track under the identity it was saved with, so that
            // anything keyed by it (ARA persistent IDs, frozen track state)
            // still resolves after a reload.
            host_.stageTrackReferenceId(tracks[i]->referenceId());
            const int32_t trackIndex = engine_.addEmptyTrack();
            host_.stageTrackReferenceId({});
            if (trackIndex < 0) {
                run.error = "Failed to create track";
                return;
            }

            queuePluginLoadsForTrack(run, tracks[i], trackIndex);

            for (auto& clip : tracks[i]->clips()) {
                if (!clip)
                    continue;
                if (!restoreTrackClip(run, *clip, trackIndex))
                    return;
            }
        }
    }

    // Recreates one clip on an already-created track. Returns false when the
    // load cannot continue, having set run.error.
    bool TimelineProjectSerializer::restoreTrackClip(
        ProjectLoadRun& run,
        UapmdProjectClipData& clip,
        int32_t trackIndex) {
        // Staged per clip, so a clip that fails to load cannot leak its
        // identity onto the next one.
        host_.stageClipReferenceId(clip.referenceId());

        TimelinePosition position;
        position.samples = static_cast<int64_t>(clip.absolutePositionInSamples());

        const auto clipType = clip.clipType();
        std::filesystem::path resolvedPath = clip.file();
        if (!resolvedPath.empty())
            resolvedPath = makeAbsolutePath(run.dir, resolvedPath);

        auto* timelineTrack = facade_.tracks()[static_cast<size_t>(trackIndex)];

        if (clipType != "midi") {
            std::unique_ptr<AudioFileReader> reader;
            std::string filepath;
            if (resolvedPath.empty()) {
                // A missing audio file is replaced by silence of the recorded
                // length, so the rest of the project still loads.
                const int64_t durationSamples = std::max<int64_t>(
                    1,
                    clip.durationSamples() > 0
                        ? clip.durationSamples()
                        : static_cast<int64_t>(host_.sampleRate()));
                const uint32_t channelCount = std::max<uint32_t>(1, timelineTrack->channelCount());
                reader = std::make_unique<SilentAudioFileReader>(
                    static_cast<uint64_t>(durationSamples),
                    channelCount,
                    static_cast<uint32_t>(host_.sampleRate()));
            } else {
                reader = createAudioFileReaderFromPath(resolvedPath.string());
                if (!reader) {
                    run.error = std::format("Failed to open audio clip {}", resolvedPath.string());
                    return false;
                }
                filepath = resolvedPath.string();
            }
            auto loadResult = host_.addAudioClipToTimelineTrack(
                *timelineTrack, position, std::move(reader), filepath,
                clip.markers(), clip.audioWarps());
            if (!loadResult.success) {
                run.error = loadResult.error.empty() ? "Failed to load audio clip" : loadResult.error;
                return false;
            }
            auto* loadedClip = timelineTrack->clipManager().getClip(loadResult.clipId);
            run.loadedClips[&clip] = LoadedClipRef{
                timelineTrack,
                loadResult.clipId,
                loadedClip ? loadedClip->referenceId : std::string{}};
            return true;
        }

        if (resolvedPath.empty()) {
            run.error = "MIDI clip is missing file path";
            return false;
        }
        auto clipInfo = MidiClipReader::readAnyFormat(resolvedPath);
        if (!clipInfo.success) {
            run.error = clipInfo.error.empty() ? "Failed to parse MIDI clip" : clipInfo.error;
            return false;
        }

        // A MIDI file mixes musical events with tempo/time-signature meta,
        // which belongs on the master track.
        auto separated = MidiClipReader::separateMasterTrackEvents(std::move(clipInfo));
        auto& musicalClip = separated.musicalClip;
        auto& masterClip = separated.masterTrackClip;

        if (separated.hasMusicalClip()) {
            double clipTempo = musicalClip.tempo_changes.empty()
                ? 120.0 : musicalClip.tempo_changes.front().bpm;
            if (clipTempo <= 0.0)
                clipTempo = 120.0;
            auto loadResult = facade_.addMidiClipToTrack(
                trackIndex, position,
                std::move(musicalClip.ump_data),
                std::move(musicalClip.ump_tick_timestamps),
                musicalClip.tick_resolution,
                clipTempo,
                std::move(musicalClip.tempo_changes),
                std::move(musicalClip.time_signature_changes),
                resolvedPath.filename().string(),
                clip.nrpnToParameterMapping(),
                separated.hasMasterTrackClip(),
                ProjectMutationOrigin::Load);
            if (!loadResult.success) {
                run.error = loadResult.error.empty() ? "Failed to load MIDI clip" : loadResult.error;
                return false;
            }
            auto* loadedClip = timelineTrack->clipManager().getClip(loadResult.clipId);
            if (loadedClip) {
                loadedClip->markers = clip.markers();
                loadedClip->audioWarps = clip.audioWarps();
            }
            run.loadedClips[&clip] = LoadedClipRef{
                timelineTrack,
                loadResult.clipId,
                loadedClip ? loadedClip->referenceId : std::string{}};
        }

        // Only synthesise a master-track clip when the project did not save
        // its own; otherwise the saved one is authoritative.
        if (!run.hasExplicitMasterTrackClips && separated.hasMasterTrackClip()) {
            auto masterLoadResult = facade_.addMasterMidiClip(
                position, {}, {},
                masterClip.tick_resolution,
                masterClip.tempo,
                std::move(masterClip.tempo_changes),
                std::move(masterClip.time_signature_changes),
                std::format("{} Meta", resolvedPath.filename().string()),
                false, "",
                ProjectMutationOrigin::Load);
            if (!masterLoadResult.success) {
                run.error = masterLoadResult.error.empty()
                    ? "Failed to load master track clip" : masterLoadResult.error;
                return false;
            }
            // Anchor the meta clip to the clip it came from, so they move
            // together.
            auto refIt = run.loadedClips.find(&clip);
            if (refIt != run.loadedClips.end() && !refIt->second.clipReferenceId.empty())
                facade_.masterTimelineTrack()->clipManager().setClipAnchor(
                    masterLoadResult.clipId,
                    TimeReference::fromContainerStart(refIt->second.clipReferenceId, 0.0),
                    host_.sampleRate());
        }
        return true;
    }

    // The master track carries the project's tempo and time-signature map.
    void TimelineProjectSerializer::restoreMasterTrackClips(ProjectLoadRun& run) {
        if (!run.error.empty() || !run.masterTrack)
            return;
        queuePluginLoadsForTrack(run, run.masterTrack, kMasterTrackIndex);
        for (auto& clip : run.masterTrack->clips()) {
            if (!clip || clip->clipType() != "midi")
                continue;
            host_.stageClipReferenceId(clip->referenceId());
            auto resolvedPath = makeAbsolutePath(run.dir, clip->file());
            if (resolvedPath.empty())
                continue;
            auto clipInfo = MidiClipReader::readAnyFormat(resolvedPath);
            if (!clipInfo.success)
                continue;
            double clipTempo = clipInfo.tempo_changes.empty()
                ? 120.0 : clipInfo.tempo_changes.front().bpm;
            if (clipTempo <= 0.0)
                clipTempo = 120.0;
            if (!clipInfo.tempo_changes.empty())
                facade_.state().tempo = clipTempo;
            TimelinePosition pos;
            pos.samples = static_cast<int64_t>(clip->absolutePositionInSamples());
            auto masterLoadResult = facade_.addMasterMidiClip(
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
                run.error = masterLoadResult.error.empty()
                    ? "Failed to load master track clip" : masterLoadResult.error;
                return;
            }
            auto* loadedClip =
                facade_.masterTimelineTrack()->clipManager().getClip(masterLoadResult.clipId);
            if (loadedClip) {
                loadedClip->markers = clip->markers();
                loadedClip->audioWarps = clip->audioWarps();
            }
            run.loadedClips[clip.get()] = LoadedClipRef{
                facade_.masterTimelineTrack(),
                masterLoadResult.clipId,
                loadedClip ? loadedClip->referenceId : std::string{}};
        }
    }

    // Anchors may reference another clip, so they can only be resolved once
    // every clip in the project exists.
    void TimelineProjectSerializer::applyLoadedClipAnchors(ProjectLoadRun& run) {
        if (!run.error.empty())
            return;

        auto applyAnchor = [this, &run](UapmdProjectClipData* projectClip) {
            if (!projectClip)
                return;
            auto loadedIt = run.loadedClips.find(projectClip);
            if (loadedIt == run.loadedClips.end())
                return;
            auto* targetTrack = loadedIt->second.track;
            if (!targetTrack)
                return;

            auto pos = projectClip->position();
            TimeReference anchor = TimeReference::fromContainerStart();
            if (auto* anchorClip = dynamic_cast<UapmdProjectClipData*>(pos.anchor)) {
                auto anchorIt = run.loadedClips.find(anchorClip);
                if (anchorIt != run.loadedClips.end())
                    anchor.referenceId = anchorIt->second.clipReferenceId;
            }
            anchor.type = pos.origin == UapmdAnchorOrigin::End
                ? TimeReferenceType::ContainerEnd
                : TimeReferenceType::ContainerStart;
            anchor.offset =
                TimelinePosition(static_cast<int64_t>(pos.samples)).toSeconds(host_.sampleRate());
            targetTrack->clipManager().setClipAnchor(
                loadedIt->second.clipId, anchor, host_.sampleRate());
            targetTrack->clipManager().setClipPosition(
                loadedIt->second.clipId,
                TimelinePosition(static_cast<int64_t>(projectClip->absolutePositionInSamples())));
        };

        for (auto* projectTrack : run.project->tracks())
            for (auto& clip : projectTrack->clips())
                applyAnchor(clip.get());
        if (run.masterTrack)
            for (auto& clip : run.masterTrack->clips())
                applyAnchor(clip.get());
    }

    // Installs what runs once every queued plug-in has finished. This must
    // happen before the sentinel is released, because a plug-in callback that
    // observes the count reaching zero calls it immediately.
    void TimelineProjectSerializer::installLoadCompletion(ProjectLoadRun& run) {
        if (!run.error.empty()) {
            host_.setLoadInProgress(false);
            *run.finish = [callback = std::move(run.callback),
                           error = std::move(run.error)]() mutable {
                callback({false, std::move(error)});
            };
            return;
        }

        auto sharedProject = std::shared_ptr<UapmdProjectData>(std::move(run.project));
        auto finish = [this,
                       projectFile = run.file,
                       projectDir = run.dir,
                       sharedProject = std::move(sharedProject),
                       markers = std::move(run.masterTrackMarkers),
                       callback = std::move(run.callback)]() mutable {
            finalizeLoadedProject(projectFile, projectDir, sharedProject, std::move(markers), callback);
        };
        // Completion touches the document, so it always runs on the model
        // thread even when the last plug-in finished on another one.
        *run.finish = [finish = std::move(finish)]() mutable {
            if (remidy::EventLoop::runningOnMainThread())
                finish();
            else
                remidy::EventLoop::enqueueTaskOnMainThread(std::move(finish));
        };
    }

    // Everything that can only be done once every plug-in exists: graph
    // wiring, mixer state, extension data, and establishing the loaded
    // document as a clean history root.
    void TimelineProjectSerializer::finalizeLoadedProject(
        const std::filesystem::path& projectFile,
        const std::filesystem::path& projectDir,
        const std::shared_ptr<UapmdProjectData>& project,
        std::vector<ClipMarker> masterTrackMarkers,
        const TimelineFacade::ProjectLoadCallback& callback) {
        host_.setLoadInProgress(false);

        auto applyGraphConnections = [this](
            UapmdProjectTrackData* projectTrack, SequencerTrack* sequencerTrack) {
            if (!projectTrack || !sequencerTrack || !projectTrack->graph())
                return;
            materializeProjectGraph(projectTrack, sequencerTrack, engine_.umpBufferSizeInBytes());
        };

        auto& tracks = project->tracks();
        auto* masterProjectTrack = project->masterTrack();
        for (size_t i = 0; i < tracks.size() && i < engine_.tracks().size(); ++i)
            applyGraphConnections(tracks[i], engine_.tracks()[i]);
        if (masterProjectTrack)
            applyGraphConnections(masterProjectTrack, engine_.masterTrack());

        std::string projectDataError;
        if (!loadProjectDataExtensions(*project, projectDataError)) {
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
        if (masterProjectTrack && engine_.masterTrack())
            engine_.masterTrack()->trackGain(masterProjectTrack->volume());

        ProjectDocumentEvent loadedEvent(
            ProjectDocumentEventKind::ProjectLoaded, "project-loaded");
        loadedEvent.setProjectId(projectFile.string())
            .setFullResyncRecommended(true)
            .setDetail("source.file", projectFile.string());
        host_.emitProjectDocumentEvent(std::move(loadedEvent));
        host_.emitMasterTrackChanged("master-track-content-changed");

        std::string extensionError;
        if (!loadProjectExtensionData(projectFile, projectDir, extensionError)) {
            callback({false, std::move(extensionError)});
            return;
        }
        facade_.commands().history().clear(true);
        host_.notifyTimelineChanged();
        engine_.setMasterTrackMarkers(std::move(masterTrackMarkers));
        callback({true, {}});
    }

    // Runs the queued instantiations one at a time. The extra count taken here
    // is released by the last step, so completion cannot fire early.
    void TimelineProjectSerializer::runQueuedPluginLoads(ProjectLoadRun& run) {
        if (!run.error.empty() || run.pluginSteps->empty())
            return;

        run.pendingPlugins->fetch_add(1, std::memory_order_relaxed);
        auto nextIndex = std::make_shared<size_t>(0);
        auto runNext = std::make_shared<std::function<void()>>();
        *runNext = [steps = run.pluginSteps, nextIndex, runNext,
                    pending = run.pendingPlugins, finish = run.finish]() mutable {
            if (*nextIndex >= steps->size()) {
                if (pending->fetch_sub(1, std::memory_order_acq_rel) == 1)
                    (*finish)();
                return;
            }
            auto step = (*steps)[(*nextIndex)++];
            step([runNext]() mutable {
                (*runNext)();
            });
        };
        (*runNext)();
    }

    void TimelineProjectSerializer::queueProjectGraphSerialization(
            PendingProjectSaveContext& operation,
            SequencerTrack* sequencerTrack,
            UapmdProjectTrackData& projectTrack,
            const std::string& scopeLabel) {
                const auto* graphProvider = sequencerTrack
            ? facade_.audioGraphProviderRegistry().get(sequencerTrack->graph())
            : facade_.audioGraphProviderRegistry().get("");
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

    bool TimelineProjectSerializer::materializeProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            size_t eventBufferSizeInBytes) {
                if (!projectTrack || !sequencerTrack || !projectTrack->graph())
            return true;

        auto* provider = facade_.audioGraphProviderRegistry().get(projectTrack->graph()->graphType());
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
        if (!facade_.commands().replaceTrackGraphType(
                trackIndex,
                provider->id(),
                eventBufferSizeInBytes,
                ProjectMutationOrigin::Internal))
            return false;
        return provider->deserializeRuntimeGraph(
            projectTrack->graph(), sequencerTrack->graph(), sequencerTrack->orderedInstanceIds());
    }

    bool TimelineProjectSerializer::saveProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            const std::filesystem::path& projectDir,
            const std::filesystem::path& graphDir,
            const std::string& scopeLabel,
            std::string& error) {
                if (!projectTrack || !sequencerTrack)
            return true;

        auto* provider = facade_.audioGraphProviderRegistry().get(sequencerTrack->graph());
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

    bool TimelineProjectSerializer::saveProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        extensions = host_.serializationExtensions();
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

    bool TimelineProjectSerializer::loadProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        extensions = host_.serializationExtensions();
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

    bool TimelineProjectSerializer::saveProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        extensions = host_.serializationExtensions();

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

    bool TimelineProjectSerializer::loadProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) {
                std::vector<ProjectSerializationExtension*> extensions;
        extensions = host_.serializationExtensions();

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

    bool writeBinaryFile(
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


} // namespace uapmd::timeline_detail
