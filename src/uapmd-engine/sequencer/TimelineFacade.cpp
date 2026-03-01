#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <unordered_map>

#include "uapmd-engine/uapmd-engine.hpp"
#include "uapmd-data/priv/timeline/AudioFileSourceNode.hpp"
#include "uapmd-data/priv/timeline/MidiClipSourceNode.hpp"
#include "uapmd-data/priv/audio/AudioFileFactory.hpp"
#include "uapmd-data/priv/project/UapmdProjectFile.hpp"
#include "uapmd-data/priv/project/MidiClipReader.hpp"

namespace uapmd {

    class TimelineFacadeImpl : public TimelineFacade {
        SequencerEngine& engine_;
        int32_t sampleRate_;
        uint32_t bufferSizeInFrames_;

        using TrackList = std::vector<std::shared_ptr<TimelineTrack>>;
        TrackList timeline_tracks_;                          // UI-thread owned
        std::shared_ptr<const TrackList> timeline_tracks_snapshot_; // RT-thread read via atomic

        void rebuildTrackSnapshot() {
            auto snap = std::make_shared<TrackList>(timeline_tracks_);
            std::atomic_store_explicit(&timeline_tracks_snapshot_,
                std::shared_ptr<const TrackList>(snap), std::memory_order_release);
        }

        TimelineState timeline_;
        int32_t next_source_node_id_{1};

    public:
        explicit TimelineFacadeImpl(SequencerEngine& engine)
            : engine_(engine)
            , sampleRate_(0)
            , bufferSizeInFrames_(0)
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

        ClipAddResult addClipToTrack(
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
            clip.anchorClipId = -1;
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
            const std::string& filepath) override
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

            int32_t sourceNodeId = next_source_node_id_++;
            auto sourceNode = std::make_unique<MidiClipSourceNode>(
                sourceNodeId,
                std::move(clipInfo.ump_data),
                std::move(clipInfo.ump_tick_timestamps),
                clipInfo.tick_resolution,
                clipInfo.tempo,
                static_cast<double>(sampleRate_),
                std::move(clipInfo.tempo_changes),
                std::move(clipInfo.time_signature_changes)
            );

            int64_t durationSamples = sourceNode->totalLength();

            ClipData clip;
            clip.clipType = ClipType::Midi;
            clip.position = position;
            clip.durationSamples = durationSamples;
            clip.sourceNodeInstanceId = sourceNodeId;
            clip.filepath = filepath;
            clip.tickResolution = clipInfo.tick_resolution;
            clip.clipTempo = clipInfo.tempo;
            clip.gain = 1.0;
            clip.muted = false;
            clip.name = std::filesystem::path(filepath).stem().string();
            clip.anchorClipId = -1;
            clip.anchorOrigin = AnchorOrigin::Start;
            clip.anchorOffset = position;

            int32_t clipId = timeline_tracks_[static_cast<size_t>(trackIndex)]->addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
            } else {
                result.error = "Failed to add MIDI clip to track";
            }
            return result;
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
            const std::string& clipName) override
        {
            ClipAddResult result;
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
                result.error = "Invalid track index";
                return result;
            }

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
            clip.filepath = "";
            clip.tickResolution = tickResolution;
            clip.clipTempo = clipTempo;
            clip.gain = 1.0;
            clip.muted = false;
            clip.name = clipName.empty() ? "MIDI Clip" : clipName;
            clip.anchorClipId = -1;
            clip.anchorOrigin = AnchorOrigin::Start;
            clip.anchorOffset = position;

            int32_t clipId = timeline_tracks_[static_cast<size_t>(trackIndex)]->addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
            } else {
                result.error = "Failed to add MIDI clip to track";
            }
            return result;
        }

        bool removeClipFromTrack(int32_t trackIndex, int32_t clipId) override {
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

            auto loadPluginsForTrack = [this, &projectDir, &pending_plugins](UapmdProjectTrackData* projectTrack, int32_t trackIndex) {
                if (!projectTrack)
                    return;
                auto* graphData = projectTrack->graph();
                if (!graphData)
                    return;
                for (const auto& plugin : graphData->plugins()) {
                    if (plugin.plugin_id.empty() || plugin.format.empty())
                        continue;
                    std::string format = plugin.format;
                    std::string pluginId = plugin.plugin_id;
                    std::string stateFile = plugin.state_file;
                    std::filesystem::path resolvedState;
                    if (!stateFile.empty())
                        resolvedState = makeAbsolutePath(projectDir, stateFile);

                    pending_plugins.fetch_add(1, std::memory_order_relaxed);
                    engine_.addPluginToTrack(trackIndex, format, pluginId,
                        [this, resolvedState, &pending_plugins](int32_t instanceId, int32_t, std::string error) {
                            if (error.empty() && instanceId >= 0 && !resolvedState.empty()) {
                                auto* instance = engine_.getPluginInstance(instanceId);
                                if (instance) {
                                    std::ifstream f(resolvedState, std::ios::binary);
                                    if (f) {
                                        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
                                        instance->loadState(data);
                                    }
                                }
                            }
                            pending_plugins.fetch_sub(1, std::memory_order_release);
                        });
                }
            };

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
                        double clipTempo = clipInfo.tempo_changes.empty() ? 120.0 : clipInfo.tempo_changes.front().bpm;
                        if (clipTempo <= 0.0) clipTempo = 120.0;
                        auto loadResult = addMidiClipToTrack(
                            trackIndex, position,
                            std::move(clipInfo.ump_data),
                            std::move(clipInfo.ump_tick_timestamps),
                            clipInfo.tick_resolution,
                            clipTempo,
                            std::move(clipInfo.tempo_changes),
                            std::move(clipInfo.time_signature_changes),
                            resolvedPath.filename().string());
                        if (!loadResult.success) {
                            result.error = loadResult.error.empty() ? "Failed to load MIDI clip" : loadResult.error;
                            return result;
                        }
                    } else {
                        auto reader = createAudioFileReaderFromPath(resolvedPath.string());
                        if (!reader) {
                            result.error = std::format("Failed to open audio clip {}", resolvedPath.string());
                            return result;
                        }
                        auto loadResult = addClipToTrack(trackIndex, position, std::move(reader), resolvedPath.string());
                        if (!loadResult.success) {
                            result.error = loadResult.error.empty() ? "Failed to load audio clip" : loadResult.error;
                            return result;
                        }
                    }
                }
            }

            // Load master track clips (tempo/time-signature map)
            if (auto* masterProjectTrack = project->masterTrack()) {
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
                    // Store master clip data into timeline track 0 so buildMasterTrackSnapshot can read it
                    if (!timeline_tracks_.empty()) {
                        TimelinePosition pos;
                        pos.samples = static_cast<int64_t>(clip->absolutePositionInSamples());
                        addMidiClipToTrack(0, pos,
                            std::move(clipInfo.ump_data),
                            std::move(clipInfo.ump_tick_timestamps),
                            clipInfo.tick_resolution,
                            clipTempo,
                            std::move(clipInfo.tempo_changes),
                            std::move(clipInfo.time_signature_changes),
                            resolvedPath.filename().string());
                    }
                }
            }

            // Wait for all async plugin instantiations to complete
            while (pending_plugins.load(std::memory_order_acquire) > 0)
                std::this_thread::yield();

            result.success = true;
            return result;
        }

        MasterTrackSnapshot buildMasterTrackSnapshot() override {
            MasterTrackSnapshot snapshot;
            const double sr = std::max(1.0, static_cast<double>(sampleRate_));

            for (const auto& trackPtr : timeline_tracks_) {
                if (!trackPtr)
                    continue;
                auto clips = trackPtr->clipManager().getAllClips();
                if (clips.empty())
                    continue;

                std::unordered_map<int32_t, const ClipData*> clipMap;
                clipMap.reserve(clips.size());
                for (auto& clip : clips)
                    clipMap[clip.clipId] = &clip;

                for (const auto& clip : clips) {
                    if (clip.clipType != ClipType::Midi)
                        continue;
                    auto sourceNode = trackPtr->getSourceNode(clip.sourceNodeInstanceId);
                    auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                    if (!midiNode)
                        continue;

                    const auto absolutePosition = clip.getAbsolutePosition(clipMap);
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
            }

            std::sort(snapshot.tempoPoints.begin(), snapshot.tempoPoints.end(),
                [](const MasterTrackSnapshot::TempoPoint& a, const MasterTrackSnapshot::TempoPoint& b) {
                    return a.timeSeconds < b.timeSeconds;
                });
            std::sort(snapshot.timeSignaturePoints.begin(), snapshot.timeSignaturePoints.end(),
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

                std::unordered_map<int32_t, const ClipData*> clipMap;
                clipMap.reserve(clips.size());
                for (auto& clip : clips)
                    clipMap[clip.clipId] = &clip;

                for (const auto& clip : clips) {
                    auto absolute = clip.getAbsolutePosition(clipMap);
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

        void processTracksAudio(AudioProcessContext& process) override {
            // Hold a snapshot reference for the duration of this callback so that
            // tracks added or removed on the UI thread cannot destroy TrackList
            // elements while we are iterating them.
            auto snapshot = std::atomic_load_explicit(
                &timeline_tracks_snapshot_, std::memory_order_acquire);

            // Advance playhead and sync MasterContext
            if (timeline_.isPlaying) {
                timeline_.playheadPosition.samples += process.frameCount();

                if (timeline_.loopEnabled &&
                    timeline_.playheadPosition.samples >= timeline_.loopEnd.samples)
                    timeline_.playheadPosition.samples = timeline_.loopStart.samples;

                // Update tempo from MIDI clips on track 0 (if any)
                if (snapshot && !snapshot->empty()) {
                    auto* t = (*snapshot)[0].get();
                    if (t) {
                        auto activeClips = t->clipManager().getActiveClipsAt(timeline_.playheadPosition);
                        for (const auto& clip : activeClips) {
                            if (clip.clipType != ClipType::Midi || clip.muted)
                                continue;
                            auto sourceNode = t->getSourceNode(clip.sourceNodeInstanceId);
                            auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                            if (!midiNode)
                                continue;
                            const auto& tempoSamples = midiNode->tempoChangeSamples();
                            const auto& tempoEvents = midiNode->tempoChanges();
                            if (!tempoEvents.empty() && tempoEvents.size() == tempoSamples.size()) {
                                std::unordered_map<int32_t, const ClipData*> clipMap;
                                auto allClips = t->clipManager().getAllClips();
                                for (auto& c : allClips) clipMap[c.clipId] = &c;
                                int64_t sourcePos = clip.getSourcePosition(timeline_.playheadPosition, clipMap);
                                if (sourcePos >= 0) {
                                    double currentTempo = tempoEvents[0].bpm;
                                    for (size_t i = 0; i < tempoSamples.size(); ++i) {
                                        if (static_cast<int64_t>(tempoSamples[i]) <= sourcePos)
                                            currentTempo = tempoEvents[i].bpm;
                                        else
                                            break;
                                    }
                                    timeline_.tempo = currentTempo;
                                }
                            }
                            const auto& sigSamples = midiNode->timeSignatureChangeSamples();
                            const auto& sigEvents = midiNode->timeSignatureChanges();
                            if (!sigEvents.empty() && sigEvents.size() == sigSamples.size()) {
                                std::unordered_map<int32_t, const ClipData*> clipMap;
                                auto allClips = t->clipManager().getAllClips();
                                for (auto& c : allClips) clipMap[c.clipId] = &c;
                                int64_t sourcePos = clip.getSourcePosition(timeline_.playheadPosition, clipMap);
                                if (sourcePos >= 0) {
                                    uint8_t num = sigEvents[0].numerator;
                                    uint8_t den = sigEvents[0].denominator;
                                    for (size_t i = 0; i < sigSamples.size(); ++i) {
                                        if (static_cast<int64_t>(sigSamples[i]) <= sourcePos) {
                                            num = sigEvents[i].numerator;
                                            den = sigEvents[i].denominator;
                                        } else break;
                                    }
                                    timeline_.timeSignatureNumerator = num;
                                    timeline_.timeSignatureDenominator = den;
                                }
                            }
                            break;
                        }
                    }
                }
            }

            // Update legacy_beats
            double secondsPerBeat = 60.0 / timeline_.tempo;
            int64_t samplesPerBeat = static_cast<int64_t>(secondsPerBeat * sampleRate_);
            if (samplesPerBeat > 0) {
                timeline_.playheadPosition.legacy_beats =
                    static_cast<double>(timeline_.playheadPosition.samples) / static_cast<double>(samplesPerBeat);
            }

            // Sync to MasterContext
            auto& masterCtx = process.masterContext();
            masterCtx.playbackPositionSamples(timeline_.playheadPosition.samples);
            masterCtx.isPlaying(timeline_.isPlaying);
            uint32_t tempoMicros = static_cast<uint32_t>(60000000.0 / timeline_.tempo);
            masterCtx.tempo(tempoMicros);
            masterCtx.timeSignatureNumerator(timeline_.timeSignatureNumerator);
            masterCtx.timeSignatureDenominator(timeline_.timeSignatureDenominator);

            // Process each timeline track into its sequencer track context
            auto& sequenceData = engine_.data();
            if (!snapshot) return;
            for (size_t i = 0; i < snapshot->size() && i < sequenceData.tracks.size(); ++i) {
                auto* trackContext = sequenceData.tracks[i];
                if (!trackContext)
                    continue;

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
                            std::memcpy(dst, src, process.frameCount() * sizeof(float));
                    }
                }

                (*snapshot)[i]->processAudio(*trackContext, timeline_);
            }
        }

        void onTrackAdded(uint32_t outputChannels, double sampleRate, uint32_t bufferSizeInFrames) override {
            sampleRate_ = static_cast<int32_t>(sampleRate);
            bufferSizeInFrames_ = bufferSizeInFrames;
            timeline_tracks_.emplace_back(std::make_shared<TimelineTrack>(
                outputChannels, sampleRate, bufferSizeInFrames));
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
