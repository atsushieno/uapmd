#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <unordered_map>

#include "remidy/remidy.hpp"
#include "uapmd-data/uapmd-data.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    class TimelineFacadeImpl : public TimelineFacade {
        SequencerEngine& engine_;
        int32_t sampleRate_;
        uint32_t bufferSizeInFrames_;

        using TrackList = std::vector<std::shared_ptr<TimelineTrack>>;
        TrackList timeline_tracks_;                          // UI-thread owned
        std::shared_ptr<const TrackList> timeline_tracks_snapshot_; // RT-thread read via atomic
        std::shared_ptr<TimelineTrack> master_timeline_track_;

        void rebuildTrackSnapshot() {
            auto snap = std::make_shared<TrackList>(timeline_tracks_);
            std::atomic_store_explicit(&timeline_tracks_snapshot_,
                std::shared_ptr<const TrackList>(snap), std::memory_order_release);
        }

        TimelineState timeline_;
        int32_t next_source_node_id_{1};
        uint32_t next_timeline_track_reference_{1};

    public:
        explicit TimelineFacadeImpl(SequencerEngine& engine)
            : engine_(engine)
            , sampleRate_(0)
            , bufferSizeInFrames_(0)
            , master_timeline_track_(std::make_shared<TimelineTrack>(std::string("master_track"), 0, 44100.0, 0))
        {
            timeline_.tempo = 120.0;
            timeline_.timeSignatureNumerator = 4;
            timeline_.timeSignatureDenominator = 4;
            timeline_.isPlaying = false;
            timeline_.loopEnabled = false;
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
            clip.anchorReferenceId.clear();
            clip.anchorOrigin = AnchorOrigin::Start;
            clip.anchorOffset = position;
            clip.nrpnToParameterMapping = nrpnToParameterMapping;

            int32_t clipId = timelineTrack.addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
            } else {
                result.error = "Failed to add MIDI clip to track";
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
            if (!reader) {
                result.error = "Invalid audio file reader";
                return result;
            }

            int32_t sourceNodeId = next_source_node_id_++;
            auto sourceNode = std::make_unique<AudioFileSourceNode>(
                sourceNodeId,
                std::move(reader),
                static_cast<double>(sampleRate_)
            );

            int64_t durationSamples = sourceNode->totalLength();

            ClipData clip;
            clip.position = position;
            clip.durationSamples = durationSamples;
            clip.sourceNodeInstanceId = sourceNodeId;
            clip.gain = 1.0;
            clip.muted = false;
            clip.filepath = filepath;
            clip.anchorReferenceId.clear();
            clip.anchorOrigin = AnchorOrigin::Start;
            clip.anchorOffset = position;

            int32_t clipId = timeline_tracks_[static_cast<size_t>(trackIndex)]->addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
            } else {
                result.error = "Failed to add clip to track";
            }
            return result;
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
            if (trackIndex == kMasterTrackIndex)
                return master_timeline_track_ ? master_timeline_track_->removeClip(clipId) : false;
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size()))
                return false;
            return timeline_tracks_[static_cast<size_t>(trackIndex)]->removeClip(clipId);
        }

        ProjectResult loadProject(const std::filesystem::path& projectFile) override {
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
                sampleRate_ > 0 ? static_cast<double>(sampleRate_) : 44100.0,
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

            auto loadPluginsForTrack = [this, &projectDir, &pending_plugins](UapmdProjectTrackData* projectTrack, int32_t trackIndex) {
                if (!projectTrack)
                    return;
                auto* graphData = projectTrack->graph();
                if (!graphData)
                    return;
                auto plugins = graphData->plugins();
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

                for (const auto& plugin : plugins) {
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
                                            auto loadPromise = std::make_shared<std::promise<std::string>>();
                                            auto loadFuture = loadPromise->get_future();
                                            instance->loadState(std::move(data), uapmd::StateContextType::Project, false, nullptr,
                                                                [loadPromise](std::string error, void* callbackContext) {
                                                                    loadPromise->set_value(std::move(error));
                                                                });
                                            auto loadError = loadFuture.get();
                                            if (!loadError.empty()) {
                                                std::cerr << "Warning: Failed to restore plugin state for " << pluginLabel
                                                          << ": " << loadError << std::endl;
                                            }
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
                        auto loadResult = addAudioClipToTrack(trackIndex, position, std::move(reader), resolvedPath.string());
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

                std::string anchorReferenceId;
                if (auto* anchorClip = dynamic_cast<UapmdProjectClipData*>(pos.anchor)) {
                    auto anchorIt = loadedClipRefs.find(anchorClip);
                    if (anchorIt != loadedClipRefs.end()) {
                        anchorReferenceId = anchorIt->second.clipReferenceId;
                    }
                }
                auto anchorOrigin = pos.origin == UapmdAnchorOrigin::End
                    ? AnchorOrigin::End
                    : AnchorOrigin::Start;
                targetTrack->clipManager().setClipAnchor(
                    loadedIt->second.clipId,
                    anchorReferenceId,
                    anchorOrigin,
                    TimelinePosition(static_cast<int64_t>(pos.samples)));
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
            while (pending_plugins.load(std::memory_order_acquire) > 0)
                std::this_thread::yield();

            result.success = true;
            return result;
        }

        MasterTrackSnapshot buildMasterTrackSnapshot() override {
            MasterTrackSnapshot snapshot;
            const double sr = std::max(1.0, static_cast<double>(sampleRate_));
            auto clips = master_timeline_track_->clipManager().getAllClips();
            std::sort(clips.begin(), clips.end(), [](const ClipData& a, const ClipData& b) {
                return a.clipId < b.clipId;
            });

            for (const auto& clip : clips) {
                if (clip.clipType != ClipType::Midi)
                    continue;
                auto sourceNode = master_timeline_track_->getSourceNode(clip.sourceNodeInstanceId);
                auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                if (!midiNode)
                    continue;

                const auto absolutePosition = clip.position;
                const double clipStartSamples = static_cast<double>(absolutePosition.samples);

                const auto& tempoSamples = midiNode->tempoChangeSamples();
                const auto& tempoEvents = midiNode->tempoChanges();
                const size_t tempoCount = std::min(tempoSamples.size(), tempoEvents.size());
                for (size_t i = 0; i < tempoCount; ++i) {
                    MasterTrackSnapshot::TempoPoint point;
                    point.timeSeconds = (clipStartSamples + static_cast<double>(tempoSamples[i])) / sr;
                    point.tickPosition = tempoEvents[i].tickPosition;
                    point.bpm = tempoEvents[i].bpm;
                    snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                    snapshot.tempoPoints.push_back(point);
                }

                const auto& sigSamples = midiNode->timeSignatureChangeSamples();
                const auto& sigEvents = midiNode->timeSignatureChanges();
                const size_t sigCount = std::min(sigSamples.size(), sigEvents.size());
                for (size_t i = 0; i < sigCount; ++i) {
                    MasterTrackSnapshot::TimeSignaturePoint point;
                    point.timeSeconds = (clipStartSamples + static_cast<double>(sigSamples[i])) / sr;
                    point.tickPosition = sigEvents[i].tickPosition;
                    point.signature = sigEvents[i];
                    snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                    snapshot.timeSignaturePoints.push_back(point);
                }
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

        bool trackRequiresOutputAlignment(int32_t trackIndex) override {
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

            timeline_tracks_.emplace_back(std::move(newTrack));
            rebuildTrackSnapshot();
        }

        void onTrackRemoved(size_t trackIndex) override {
            if (trackIndex < timeline_tracks_.size()) {
                timeline_tracks_.erase(timeline_tracks_.begin() + static_cast<long>(trackIndex));
                rebuildTrackSnapshot();
            }
        }

    private:
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
    };

    std::unique_ptr<TimelineFacade> TimelineFacade::create(SequencerEngine& engine) {
        return std::make_unique<TimelineFacadeImpl>(engine);
    }

} // namespace uapmd
