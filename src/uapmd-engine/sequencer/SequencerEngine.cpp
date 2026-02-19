#include "uapmd/uapmd.hpp"
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <umppi/umppi.hpp>
#include <unordered_set>

#include <remidy/remidy.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {
    class SequencerEngineImpl : public SequencerEngine {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        uint32_t default_input_channels_{2};
        uint32_t default_output_channels_{2};
        std::vector<std::unique_ptr<SequencerTrack>> tracks_{};
        std::unique_ptr<SequencerTrack> master_track_;
        std::unique_ptr<AudioProcessContext> master_track_context_;
        SequenceProcessContext sequence{};
        int32_t sampleRate;
        std::unique_ptr<AudioPluginHostingAPI> plugin_host;
        UapmdFunctionBlockManager function_block_manager{};

        // Playback state (managed by RealtimeSequencer)
        std::atomic<bool> is_playback_active_{false};
        std::atomic<int64_t> playback_position_samples_{0};

        // Audio preprocessing callback (for app-level source nodes)
        AudioPreprocessCallback audio_preprocess_callback_;

        // Audio analysis
        static constexpr int kSpectrumBars = 32;
        // RT-thread local buffers (no lock needed)
        float rt_input_spectrum_[kSpectrumBars] = {};
        float rt_output_spectrum_[kSpectrumBars] = {};
        // Shared buffers for non-RT readers (lock-free using atomic flag)
        float shared_input_spectrum_[kSpectrumBars] = {};
        float shared_output_spectrum_[kSpectrumBars] = {};
        // Lock-free flag: true = reader owns, false = writer can write
        mutable std::atomic<bool> spectrum_reading_{false};

        // UMP output processing
        std::vector<uapmd_ump_t> plugin_output_scratch_;

        // Plugin instance management
        std::unordered_map<int32_t, AudioPluginInstanceAPI*> plugin_instances_;
        std::mutex instance_map_mutex_;


        // Offline rendering mode
        std::atomic<bool> offline_rendering_{false};

        // Track processing flags for safe deletion (parallel to tracks_ vector)
        // Note: std::atomic is not copyable, so we use unique_ptr
        std::vector<std::unique_ptr<std::atomic<bool>>> track_processing_flags_;

        // Timeline tracks (parallel to tracks_ — one TimelineTrack per SequencerTrack)
        std::vector<std::unique_ptr<TimelineTrack>> timeline_tracks_;
        TimelineState timeline_;
        int32_t next_source_node_id_{1};

    public:
        explicit SequencerEngineImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts);

        AudioPluginHostingAPI* pluginHost() override;

        SequenceProcessContext& data() override { return sequence; }

        std::vector<SequencerTrack*>& tracks() const override;
        SequencerTrack* masterTrack() override;

        void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;
        uapmd_track_index_t addEmptyTrack() override;
        bool removeTrack(uapmd_track_index_t trackIndex) override;
        void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback) override;
        bool removePluginInstance(int32_t instanceId) override;

        void setAudioPreprocessCallback(AudioPreprocessCallback callback) override {
            audio_preprocess_callback_ = std::move(callback);
        }

        uapmd_status_t processAudio(AudioProcessContext& process) override;

        // Playback control
        bool isPlaybackActive() const override;
        void playbackPosition(int64_t samples) override;
        int64_t playbackPosition() const override;
        void startPlayback() override;
        void stopPlayback() override;
        void pausePlayback() override;
        void resumePlayback() override;

        // Audio analysis
        void getInputSpectrum(float* outSpectrum, int numBars) const override;
        void getOutputSpectrum(float* outSpectrum, int numBars) const override;

        // Plugin instance queries
        AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) override;

        UapmdFunctionBlockManager *functionBlockManager() override { return &function_block_manager; }
        int32_t findTrackIndexForInstance(int32_t instanceId) const override;

        // Event routing
        void enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) override;

        // Convenience methods for sending MIDI events
        void sendNoteOn(int32_t instanceId, int32_t note) override;
        void sendNoteOff(int32_t instanceId, int32_t note) override;
        void sendPitchBend(int32_t instanceId, float normalizedValue) override;
        void sendChannelPressure(int32_t instanceId, float pressure) override;

        void setParameterValue(int32_t instanceId, int32_t index, double value) override;

        bool offlineRendering() const override;
        void offlineRendering(bool enabled) override;

        void cleanupEmptyTracks() override;

        // Timeline track management
        TimelineState& timeline() override { return timeline_; }
        std::vector<TimelineTrack*> timelineTracks() override;

        // Clip management
        ClipAddResult addClipToTrack(int32_t trackIndex, const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader, const std::string& filepath = "") override;
        ClipAddResult addMidiClipToTrack(int32_t trackIndex, const TimelinePosition& position,
            const std::string& filepath) override;
        ClipAddResult addMidiClipToTrack(int32_t trackIndex, const TimelinePosition& position,
            std::vector<uapmd_ump_t> umpEvents, std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution, double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName = "") override;
        bool removeClipFromTrack(int32_t trackIndex, int32_t clipId) override;

        // Project loading
        ProjectResult loadProject(const std::filesystem::path& file) override;
        MasterTrackSnapshot buildMasterTrackSnapshot() override;

    private:
        void processTracksAudio(AudioProcessContext& process);
        void removeTrack(size_t index);

        // Routing configuration
        void configureTrackRouting(SequencerTrack* track);
        void refreshFunctionBlockMappings();

        // Route resolution
        struct RouteResolution {
            SequencerTrack* track{nullptr};
            int32_t trackIndex{-1};
            int32_t instanceId{-1};
        };

        // Output dispatch
        void dispatchPluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes);
    };

    std::unique_ptr<SequencerEngine> SequencerEngine::create(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts) {
        return std::make_unique<SequencerEngineImpl>(sampleRate, audioBufferSizeInFrames, umpBufferSizeInInts);
    }

    // SequencerEngineImpl
    SequencerEngineImpl::SequencerEngineImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts) :
        audio_buffer_size_in_frames(audioBufferSizeInFrames),
        sampleRate(sampleRate),
        ump_buffer_size_in_ints(umpBufferSizeInInts),
        plugin_host(AudioPluginHostingAPI::create()),
        plugin_output_scratch_(umpBufferSizeInInts, 0) {
        master_track_ = SequencerTrack::create(umpBufferSizeInInts);
        master_track_context_ = std::make_unique<AudioProcessContext>(sequence.masterContext(), ump_buffer_size_in_ints);
        if (master_track_context_) {
            master_track_context_->configureMainBus(default_output_channels_, default_output_channels_, audio_buffer_size_in_frames);
        }
        configureTrackRouting(master_track_.get());

        // Initialize timeline state
        timeline_.tempo = 120.0;
        timeline_.timeSignatureNumerator = 4;
        timeline_.timeSignatureDenominator = 4;
        timeline_.isPlaying = false;
        timeline_.loopEnabled = false;

        // Register timeline processing as the default preprocess callback
        audio_preprocess_callback_ = [this](AudioProcessContext& process) {
            processTracksAudio(process);
        };
    }

    std::vector<SequencerTrack*> &SequencerEngineImpl::tracks() const {
        // Note: This requires a mutable cache for const correctness
        // Since we need to return a reference to a vector of raw pointers
        static thread_local std::vector<SequencerTrack*> track_ptrs;
        track_ptrs.clear();
        for (const auto& track : tracks_)
            track_ptrs.push_back(track.get());
        return track_ptrs;
    }

    SequencerTrack* SequencerEngineImpl::masterTrack() {
        return master_track_.get();
    }

    int32_t SequencerEngineImpl::processAudio(AudioProcessContext& process) {
        // Record start time for deadline tracking
        auto startTime = std::chrono::steady_clock::now();

        if (tracks_.size() != sequence.tracks.size())
            // FIXME: define status codes
            return 1;

        auto& data = sequence;
        auto& masterContext = data.masterContext();

        // Update playback position if playback is active
        bool isPlaybackActive = is_playback_active_.load(std::memory_order_acquire);

        // Update MasterContext with current playback state
        masterContext.playbackPositionSamples(playback_position_samples_.load(std::memory_order_acquire));
        masterContext.isPlaying(isPlaybackActive);
        masterContext.sampleRate(sampleRate);

        // Copy device input directly to track input buffers (will be overwritten by app callback if set)
        for (uint32_t t = 0, nTracks = tracks_.size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventOut().position(0); // clean up *out* events here.
            ctx->frameCount(process.frameCount());

            // Copy device input to track input buffers
            for (uint32_t i = 0; i < ctx->audioInBusCount(); i++) {
                for (uint32_t ch = 0, nCh = ctx->inputChannelCount(i); ch < nCh; ch++) {
                    float* trackDst = ctx->getFloatInBuffer(i, ch);
                    // Copy from device input if available
                    if (process.audioInBusCount() > 0 && ch < process.inputChannelCount(0)) {
                        memcpy(trackDst, (void*)process.getFloatInBuffer(0, ch), process.frameCount() * sizeof(float));
                    } else {
                        memset(trackDst, 0, process.frameCount() * sizeof(float));
                    }
                }
            }
        }

        // Call app-level preprocessing callback (for source node processing)
        if (audio_preprocess_callback_) {
            audio_preprocess_callback_(process);
        }

        // Process all tracks
        for (auto i = 0; i < sequence.tracks.size(); i++) {
            // Set processing flag BEFORE accessing sequence.tracks[i]
            track_processing_flags_[i]->store(true, std::memory_order_release);

            auto& tp = *sequence.tracks[i];
            if (!tracks_[i]->bypassed())
                tracks_[i]->graph().processAudio(tp);
            tp.eventIn().position(0); // reset

            // Clear processing flag AFTER we're done with the track context
            track_processing_flags_[i]->store(false, std::memory_order_release);
        }

        // Clear main output bus (bus 0) before mixing
        if (process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++) {
                memset(process.getFloatOutBuffer(0, ch), 0, process.frameCount() * sizeof(float));
            }
        }

        // Mix all tracks into main output bus with additive mixing
        for (uint32_t t = 0, nTracks = tracks_.size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventIn().position(0); // clean up *in* events here.

            // Mix only main bus (bus 0)
            if (process.audioOutBusCount() > 0 && ctx->audioOutBusCount() > 0) {
                // Mix matching channels only
                uint32_t numChannels = std::min(ctx->outputChannelCount(0), process.outputChannelCount(0));
                for (uint32_t ch = 0; ch < numChannels; ch++) {
                    float* dst = process.getFloatOutBuffer(0, ch);
                    const float* src = ctx->getFloatOutBuffer(0, ch);
                    // Additive mixing
                    for (uint32_t frame = 0; frame < process.frameCount(); frame++) {
                        dst[frame] += src[frame];
                    }
                }
            }
        }

        // Process master track plugins if present
        if (master_track_ && master_track_context_ && !master_track_->orderedInstanceIds().empty()) {
            auto* masterCtx = master_track_context_.get();
            masterCtx->frameCount(process.frameCount());
            masterCtx->eventIn().position(0);
            masterCtx->eventOut().position(0);

            if (masterCtx->audioInBusCount() > 0 && process.audioOutBusCount() > 0) {
                uint32_t channels = std::min(
                    static_cast<uint32_t>(masterCtx->inputChannelCount(0)),
                    static_cast<uint32_t>(process.outputChannelCount(0))
                );
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    float* dst = masterCtx->getFloatInBuffer(0, ch);
                    const float* src = process.getFloatOutBuffer(0, ch);
                    if (dst && src) {
                        std::memcpy(dst, src, process.frameCount() * sizeof(float));
                    }
                }
            }

            master_track_->graph().processAudio(*masterCtx);

            if (masterCtx->audioOutBusCount() > 0 && process.audioOutBusCount() > 0) {
                uint32_t channels = std::min(
                    static_cast<uint32_t>(masterCtx->outputChannelCount(0)),
                    static_cast<uint32_t>(process.outputChannelCount(0))
                );
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    float* dst = process.getFloatOutBuffer(0, ch);
                    const float* src = masterCtx->getFloatOutBuffer(0, ch);
                    if (dst && src) {
                        std::memcpy(dst, src, process.frameCount() * sizeof(float));
                    }
                }
            }
        }

        // Apply soft clipping to prevent harsh distortion
        if (process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++) {
                float* buffer = process.getFloatOutBuffer(0, ch);
                for (uint32_t frame = 0; frame < process.frameCount(); frame++) {
                    buffer[frame] = std::tanh(buffer[frame]);
                }
            }
        }

        // Calculate spectrum for visualization (simple magnitude binning)
        // RT-safe: write to local buffers without locking
        {
            // Calculate input spectrum from device input
            for (int bar = 0; bar < kSpectrumBars; ++bar) {
                float sum = 0.0f;
                int sampleCount = 0;

                if (process.audioInBusCount() > 0) {
                    int samplesPerBar = process.frameCount() / kSpectrumBars;
                    int startSample = bar * samplesPerBar;
                    int endSample = std::min((int)process.frameCount(), (bar + 1) * samplesPerBar);

                    for (uint32_t ch = 0; ch < process.inputChannelCount(0); ++ch) {
                        const float* buffer = process.getFloatInBuffer(0, ch);
                        for (int i = startSample; i < endSample; ++i) {
                            sum += std::abs(buffer[i]);
                            sampleCount++;
                        }
                    }
                }

                rt_input_spectrum_[bar] = sampleCount > 0 ? sum / sampleCount : 0.0f;
            }

            // Calculate output spectrum from main output
            for (int bar = 0; bar < kSpectrumBars; ++bar) {
                float sum = 0.0f;
                int sampleCount = 0;

                if (process.audioOutBusCount() > 0) {
                    int samplesPerBar = process.frameCount() / kSpectrumBars;
                    int startSample = bar * samplesPerBar;
                    int endSample = std::min((int)process.frameCount(), (bar + 1) * samplesPerBar);

                    for (uint32_t ch = 0; ch < process.outputChannelCount(0); ++ch) {
                        const float* buffer = process.getFloatOutBuffer(0, ch);
                        for (int i = startSample; i < endSample; ++i) {
                            sum += std::abs(buffer[i]);
                            sampleCount++;
                        }
                    }
                }

                rt_output_spectrum_[bar] = sampleCount > 0 ? sum / sampleCount : 0.0f;
            }

            // Try to copy to shared buffers (skip if reader is active - RT-safe, lock-free)
            bool expected = false;
            if (spectrum_reading_.compare_exchange_strong(expected, false, std::memory_order_acquire)) {
                // No reader active, safe to write
                std::copy(rt_input_spectrum_, rt_input_spectrum_ + kSpectrumBars, shared_input_spectrum_);
                std::copy(rt_output_spectrum_, rt_output_spectrum_ + kSpectrumBars, shared_output_spectrum_);
                // No need to release the flag - we keep it at false for next write
            }
        }

        if (isPlaybackActive)
            playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);

        // Check for missed audio processing deadline
        auto endTime = std::chrono::steady_clock::now();
        auto elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();

        // Calculate available time for this buffer
        double availableTimeMicros = (static_cast<double>(process.frameCount()) / static_cast<double>(sampleRate)) * 1000000.0;

        // Log warning if we exceeded the deadline
        //if (elapsedMicros > availableTimeMicros) {
        if (elapsedMicros > availableTimeMicros) {
            double cpuLoad = (static_cast<double>(elapsedMicros) / availableTimeMicros) * 100.0;
            remidy::Logger::global()->logWarning(
                "Audio deadline missed: processed %d frames in %.2f μs (available: %.2f μs, CPU load: %.1f%%)",
                process.frameCount(),
                static_cast<double>(elapsedMicros),
                availableTimeMicros,
                cpuLoad
            );
        }

        // FIXME: define status codes
        return 0;
    }

    void SequencerEngineImpl::setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) {
        default_input_channels_ = inputChannels;
        default_output_channels_ = outputChannels;
        if (master_track_context_) {
            master_track_context_->configureMainBus(default_output_channels_, default_output_channels_, audio_buffer_size_in_frames);
        }
    }

    uapmd_track_index_t SequencerEngineImpl::addEmptyTrack() {
        auto tr = SequencerTrack::create(ump_buffer_size_in_ints);
        tracks_.emplace_back(std::move(tr));
        sequence.tracks.emplace_back(new AudioProcessContext(sequence.masterContext(), ump_buffer_size_in_ints));
        track_processing_flags_.emplace_back(std::make_unique<std::atomic<bool>>(false));
        auto trackIndex = static_cast<uapmd_track_index_t>(tracks_.size() - 1);

        // Configure main bus (moved from RealtimeSequencer)
        auto trackCtx = sequence.tracks[trackIndex];
        trackCtx->configureMainBus(default_input_channels_, default_output_channels_, audio_buffer_size_in_frames);

        // Create a paired TimelineTrack
        timeline_tracks_.emplace_back(std::make_unique<TimelineTrack>(
            default_output_channels_,
            static_cast<double>(sampleRate),
            static_cast<uint32_t>(audio_buffer_size_in_frames)
        ));

        return trackIndex;
    }

    bool SequencerEngineImpl::removeTrack(uapmd_track_index_t index) {
        if (index >= tracks_.size())
            return false;
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
        track_processing_flags_.erase(track_processing_flags_.begin() + static_cast<long>(index));
        return true;
    }

    void SequencerEngineImpl::addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback) {
        const bool targetMaster = (trackIndex == kMasterTrackIndex);
        if (!targetMaster) {
            // Validate track index
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
                callback(-1, -1, std::format("Invalid track index {}", trackIndex));
                return;
            }
        }

        plugin_host->createPluginInstance(sampleRate, default_input_channels_, default_output_channels_, false, format, pluginId, [this, trackIndex, targetMaster, callback](int32_t instanceId, std::string error) {
            if (instanceId < 0) {
                callback(-1, targetMaster ? kMasterTrackIndex : trackIndex, "Could not create plugin: " + error);
                return;
            }

            // Re-validate track (may have been removed during async operation)
            if (!targetMaster) {
                if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
                    callback(-1, -1, std::format("Track {} no longer exists", trackIndex));
                    return;
                }
            }

            auto instance = plugin_host->getInstance(instanceId);
            auto* track = targetMaster ? master_track_.get() : tracks_[static_cast<size_t>(trackIndex)].get();
            if (!track) {
                callback(-1, targetMaster ? kMasterTrackIndex : trackIndex, "Track unavailable for plugin insertion");
                return;
            }

            // Append to track's graph
            auto status = track->graph().appendNodeSimple(instanceId, instance, [this,instanceId] {
                auto instance = plugin_host->getInstance(instanceId);
                instance->bypassed(true);
                plugin_host->deletePluginInstance(instanceId);
            });
            if (status != 0) {
                callback(-1, -1, std::format("Failed to append plugin to track {} (status {})", trackIndex, status));
                return;
            }

            track->orderedInstanceIds().push_back(instanceId);

            // Function block setup
            configureTrackRouting(track);

            // Plugin instance management
            {
                std::lock_guard<std::mutex> lock(instance_map_mutex_);
                plugin_instances_[instanceId] = instance;
            }

            // Parameter metadata change events are now handled in AudioPluginNode directly

            refreshFunctionBlockMappings();

            instance->bypassed(false);

            callback(instanceId, targetMaster ? kMasterTrackIndex : trackIndex, "");
        });
    }

    bool SequencerEngineImpl::removePluginInstance(int32_t instanceId) {
        // Hide and destroy UI first (if caller didn't already)
        auto* instance = getPluginInstance(instanceId);
        if (instance) {
            if (instance->hasUISupport() && instance->isUIVisible())
                instance->hideUI();
            instance->destroyUI();
        }

        // Metadata listener is unregistered automatically in AudioPluginNode destructor

        // Plugin instance cleanup
        {
            std::lock_guard<std::mutex> lock(instance_map_mutex_);
            plugin_instances_.erase(instanceId);
        }

        // Remove from track graph
        for (size_t i = 0; i < tracks_.size(); ++i) {
            auto& track = tracks_[i];
            if (!track)
                continue;
            if (track->graph().removeNodeSimple(instanceId)) {
                // NOTE: Empty tracks are intentionally left in place to avoid real-time safety issues.
                // They have minimal overhead (no plugins to process) and can be removed manually
                // by calling removeTrack() from a non-audio thread when appropriate.
                refreshFunctionBlockMappings();
                return true;
            }
        }
        if (master_track_ && master_track_->graph().removeNodeSimple(instanceId)) {
            refreshFunctionBlockMappings();
            return true;
        }
        return false;
    }

    void SequencerEngineImpl::removeTrack(size_t index) {
        if (index >= tracks_.size())
            return;
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        if (index < sequence.tracks.size()) {
            auto* ctx = sequence.tracks[index];
            sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
            delete ctx;
        }
        track_processing_flags_.erase(track_processing_flags_.begin() + static_cast<long>(index));
        if (index < timeline_tracks_.size())
            timeline_tracks_.erase(timeline_tracks_.begin() + static_cast<long>(index));
    }

    // Playback control
    bool SequencerEngineImpl::isPlaybackActive() const {
        return is_playback_active_.load(std::memory_order_acquire);
    }

    void SequencerEngineImpl::playbackPosition(int64_t samples) {
        playback_position_samples_.store(samples, std::memory_order_release);
    }

    int64_t SequencerEngineImpl::playbackPosition() const {
        return playback_position_samples_.load(std::memory_order_acquire);
    }

    void uapmd::SequencerEngineImpl::startPlayback() {
        playback_position_samples_.store(0, std::memory_order_release);
        is_playback_active_.store(true, std::memory_order_release);
    }

    void uapmd::SequencerEngineImpl::stopPlayback() {
        is_playback_active_.store(false, std::memory_order_release);
        playback_position_samples_.store(0, std::memory_order_release);
        auto flushTrackNotes = [](SequencerTrack* track) {
            if (!track)
                return;
            auto plugin_nodes = track->graph().plugins();
            for (auto& entry : plugin_nodes) {
                if (entry.second)
                    entry.second->sendAllNotesOff();
            }
        };
        for (auto& track : tracks_)
            flushTrackNotes(track.get());
        flushTrackNotes(master_track_.get());
    }

    void uapmd::SequencerEngineImpl::pausePlayback() {
        is_playback_active_.store(false, std::memory_order_release);
    }

    void uapmd::SequencerEngineImpl::resumePlayback() {
        is_playback_active_.store(true, std::memory_order_release);
    }

    // Audio analysis
    void SequencerEngineImpl::getInputSpectrum(float* outSpectrum, int numBars) const {
        // Set reading flag to prevent RT thread from writing
        spectrum_reading_.store(true, std::memory_order_release);

        for (int i = 0; i < std::min(numBars, kSpectrumBars); ++i) {
            outSpectrum[i] = shared_input_spectrum_[i];
        }

        // Release the reading flag
        spectrum_reading_.store(false, std::memory_order_release);
    }

    void SequencerEngineImpl::getOutputSpectrum(float* outSpectrum, int numBars) const {
        // Set reading flag to prevent RT thread from writing
        spectrum_reading_.store(true, std::memory_order_release);

        for (int i = 0; i < std::min(numBars, kSpectrumBars); ++i) {
            outSpectrum[i] = shared_output_spectrum_[i];
        }

        // Release the reading flag
        spectrum_reading_.store(false, std::memory_order_release);
    }

    // Track routing configuration
    void SequencerEngineImpl::configureTrackRouting(SequencerTrack* track) {
        if (!track)
            return;
        track->graph().setGroupResolver([this](int32_t instanceId) {
            const auto fb = functionBlockManager()->getFunctionDeviceByInstanceId(instanceId);
            return fb ? fb->group() : static_cast<uint8_t>(0xFF);
        });
        track->graph().setEventOutputCallback([this](int32_t instanceId, const uapmd_ump_t* data, size_t dataSizeInBytes) {
            dispatchPluginOutput(instanceId, data, dataSizeInBytes);
        });
    }

    // Do we really need this...?
    void SequencerEngineImpl::refreshFunctionBlockMappings() {
        for (auto& track : tracks_)
            configureTrackRouting(track.get());
        configureTrackRouting(master_track_.get());
    }

    int32_t SequencerEngineImpl::findTrackIndexForInstance(int32_t instanceId) const {
        const auto& tracksRef = tracks();
        for (size_t i = 0; i < tracksRef.size(); ++i) {
            if (const auto& ids = tracksRef[i]->orderedInstanceIds();
                std::ranges::find(ids.begin(), ids.end(), instanceId) != tracksRef[i]->orderedInstanceIds().end()
            )
                return static_cast<int32_t>(i);
        }
        if (master_track_) {
            const auto& ids = master_track_->orderedInstanceIds();
            if (std::ranges::find(ids.begin(), ids.end(), instanceId) != ids.end())
                return kMasterTrackIndex;
        }
        return -1;
    }

    // Plugin output dispatch (with group rewriting + NRPN parameter extraction)
    void SequencerEngineImpl::dispatchPluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
        if (!data || bytes == 0)
            return;

        const auto fb = functionBlockManager()->getFunctionDeviceByInstanceId(instanceId);
        if (!fb)
            return;
        const auto group = fb->group();

        if (bytes > plugin_output_scratch_.size() * sizeof(uapmd_ump_t))
            return;

        auto* scratch = plugin_output_scratch_.data();
        std::memcpy(scratch, data, bytes);

        // Process UMP messages and extract parameter changes
        size_t offset = 0;
        auto* byteView = reinterpret_cast<uint8_t*>(scratch);
        while (offset + sizeof(uint32_t) <= bytes) {
            auto* words = reinterpret_cast<uint32_t*>(byteView + offset);
            uint8_t messageType = static_cast<uint8_t>(words[0] >> 28);
            auto wordCount = umppi::umpSizeInInts(messageType);
            size_t size = static_cast<size_t>(wordCount) * sizeof(uint32_t);
            if (offset + size > bytes)
                break;
            umppi::Ump ump(words[0],
                           wordCount > 1 ? words[1] : 0,
                           wordCount > 2 ? words[2] : 0,
                           wordCount > 3 ? words[3] : 0);

            // Check for NRPN messages (parameter changes)
            if (ump.getMessageType() == umppi::MessageType::MIDI2 &&
                static_cast<uint8_t>(ump.getStatusCode()) == umppi::MidiChannelStatus::NRPN) {
                uint8_t bank = ump.getMidi2NrpnMsb();
                uint8_t index = ump.getMidi2NrpnLsb();
                uint32_t value32 = ump.getMidi2NrpnData();

                // Reconstruct parameter ID: bank * 128 + index
                int32_t paramId = (bank * 128) + index;
                double value = static_cast<double>(value32) / 4294967295.0;

                // Find the AudioPluginNode and notify parameter update
                bool handled = false;
                for (const auto& track : tracks()) {
                    auto node = track->graph().getPluginNode(instanceId);
                    if (node) {
                        node->parameterUpdateEvent().notify(paramId, value);
                        handled = true;
                        break;
                    }
                }
                if (!handled && master_track_) {
                    if (auto* node = master_track_->graph().getPluginNode(instanceId)) {
                        node->parameterUpdateEvent().notify(paramId, value);
                    }
                }
            }

            // Rewrite group field
            words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group) << 24);
            offset += size;
        }
    }


    // Plugin instance queries
    AudioPluginInstanceAPI* SequencerEngineImpl::getPluginInstance(int32_t instanceId) {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        auto it = plugin_instances_.find(instanceId);
        if (it != plugin_instances_.end())
            return it->second;
        return nullptr;
    }

    // UMP routing
    void SequencerEngineImpl::enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        for (const auto& track : tracks())
            if (const auto node = track->graph().getPluginNode(instanceId))
                node->scheduleEvents(timestamp, ump, sizeInBytes);
        if (master_track_) {
            if (const auto node = master_track_->graph().getPluginNode(instanceId))
                node->scheduleEvents(timestamp, ump, sizeInBytes);
        }
    }

    void SequencerEngineImpl::sendNoteOn(int32_t instanceId, int32_t note) {
        uapmd_ump_t umps[2];
        // FIXME: group is dummy, to be replaced by group for instanceId (see `enqueueUmp()` comment)
        auto ump = umppi::UmpFactory::midi2NoteOn(0, 0, note, 0, 0xF800, 0);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::sendNoteOff(int32_t instanceId, int32_t note) {
        uapmd_ump_t umps[2];
        // FIXME: group is dummy, to be replaced by group for instanceId (see `enqueueUmp()` comment)
        auto ump = umppi::UmpFactory::midi2NoteOff(0, 0, note, 0, 0xF800, 0);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::sendPitchBend(int32_t instanceId, float normalizedValue) {
        uapmd_ump_t umps[2];
        float clamped = std::clamp((normalizedValue + 1.0f) * 0.5f, 0.0f, 1.0f);
        uint32_t pitchValue = static_cast<uint32_t>(clamped * 4294967295.0f);
        // FIXME: group is dummy, to be replaced by group for instanceId (see `enqueueUmp()` comment)
        auto ump = umppi::UmpFactory::midi2PitchBendDirect(0, 0, pitchValue);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::sendChannelPressure(int32_t instanceId, float pressure) {
        uapmd_ump_t umps[2];
        float clamped = std::clamp(pressure, 0.0f, 1.0f);
        uint32_t pressureValue = static_cast<uint32_t>(clamped * 4294967295.0f);
        // FIXME: group is dummy, to be replaced by group for instanceId (see `enqueueUmp()` comment)
        auto ump = umppi::UmpFactory::midi2CAf(0, 0, pressureValue);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::setParameterValue(int32_t instanceId, int32_t index, double value) {
        auto* instance = getPluginInstance(instanceId);
        if (!instance) {
            remidy::Logger::global()->logError(std::format("setParameterValue: invalid instance {}", instanceId).c_str());
            return;
        }
        instance->setParameterValue(index, value);
        remidy::Logger::global()->logInfo(std::format("Native parameter change {}: {} = {}", instanceId, index, value).c_str());
    }

    AudioPluginHostingAPI* uapmd::SequencerEngineImpl::pluginHost() {
        return plugin_host.get();
    }

    bool uapmd::SequencerEngineImpl::offlineRendering() const {
        return offline_rendering_.load(std::memory_order_acquire);
    }

    void uapmd::SequencerEngineImpl::offlineRendering(bool enabled) {
        offline_rendering_.store(enabled, std::memory_order_release);
    }

    void uapmd::SequencerEngineImpl::cleanupEmptyTracks() {
        // It uses busy-waiting to ensure the audio thread is not currently processing
        // the track before deletion.

        // Iterate backwards to preserve indices when erasing
        for (int i = static_cast<int>(tracks_.size()) - 1; i >= 0; --i) {
            auto& track = tracks_[static_cast<size_t>(i)];
            if (track && track->graph().plugins().empty()) {
                // Busy-wait until audio thread is done processing this track
                // This is typically a very short wait (microseconds to milliseconds)
                while (track_processing_flags_[static_cast<size_t>(i)]->load(std::memory_order_acquire)) {
                    // Spin-wait - audio thread will clear the flag very soon
                    std::this_thread::yield(); // Be nice to other threads
                }

                // Now safe to delete - audio thread is not using this track's context
                removeTrack(static_cast<size_t>(i));
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Timeline track management
    // ---------------------------------------------------------------------------

    std::vector<TimelineTrack*> SequencerEngineImpl::timelineTracks() {
        std::vector<TimelineTrack*> result;
        result.reserve(timeline_tracks_.size());
        for (auto& t : timeline_tracks_)
            result.push_back(t.get());
        return result;
    }

    // ---------------------------------------------------------------------------
    // Clip management
    // ---------------------------------------------------------------------------

    SequencerEngine::ClipAddResult SequencerEngineImpl::addClipToTrack(
        int32_t trackIndex, const TimelinePosition& position,
        std::unique_ptr<AudioFileReader> reader, const std::string& filepath)
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
            static_cast<double>(sampleRate)
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

    SequencerEngine::ClipAddResult SequencerEngineImpl::addMidiClipToTrack(
        int32_t trackIndex, const TimelinePosition& position, const std::string& filepath)
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
            static_cast<double>(sampleRate),
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

    SequencerEngine::ClipAddResult SequencerEngineImpl::addMidiClipToTrack(
        int32_t trackIndex, const TimelinePosition& position,
        std::vector<uapmd_ump_t> umpEvents, std::vector<uint64_t> umpTickTimestamps,
        uint32_t tickResolution, double clipTempo,
        std::vector<MidiTempoChange> tempoChanges,
        std::vector<MidiTimeSignatureChange> timeSignatureChanges,
        const std::string& clipName)
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
            static_cast<double>(sampleRate),
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

    bool SequencerEngineImpl::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size()))
            return false;
        return timeline_tracks_[static_cast<size_t>(trackIndex)]->removeClip(clipId);
    }

    // ---------------------------------------------------------------------------
    // Timeline audio processing (preprocess callback)
    // ---------------------------------------------------------------------------

    void SequencerEngineImpl::processTracksAudio(AudioProcessContext& process) {
        // Advance playhead and sync MasterContext
        if (timeline_.isPlaying) {
            timeline_.playheadPosition.samples += process.frameCount();

            if (timeline_.loopEnabled &&
                timeline_.playheadPosition.samples >= timeline_.loopEnd.samples)
                timeline_.playheadPosition.samples = timeline_.loopStart.samples;

            // Update tempo from MIDI clips on track 0 (if any)
            if (!timeline_tracks_.empty()) {
                auto* t = timeline_tracks_[0].get();
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
        int64_t samplesPerBeat = static_cast<int64_t>(secondsPerBeat * sampleRate);
        if (samplesPerBeat > 0) {
            timeline_.playheadPosition.legacy_beats =
                static_cast<double>(timeline_.playheadPosition.samples) / static_cast<double>(samplesPerBeat);
        }

        // Sync to MasterContext
        auto& masterCtx = process.trackContext()->masterContext();
        masterCtx.playbackPositionSamples(timeline_.playheadPosition.samples);
        masterCtx.isPlaying(timeline_.isPlaying);
        uint32_t tempoMicros = static_cast<uint32_t>(60000000.0 / timeline_.tempo);
        masterCtx.tempo(tempoMicros);
        masterCtx.timeSignatureNumerator(timeline_.timeSignatureNumerator);
        masterCtx.timeSignatureDenominator(timeline_.timeSignatureDenominator);

        // Process each timeline track into its sequencer track context
        auto& sequenceData = this->data();
        for (size_t i = 0; i < timeline_tracks_.size() && i < sequenceData.tracks.size(); ++i) {
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

            timeline_tracks_[i]->processAudio(*trackContext, timeline_);
        }
    }

    // ---------------------------------------------------------------------------
    // Project loading
    // ---------------------------------------------------------------------------

    namespace {
        std::filesystem::path makeAbsolutePathEngine(const std::filesystem::path& baseDir, const std::filesystem::path& target) {
            if (target.empty())
                return target;
            if (target.is_absolute() || baseDir.empty())
                return std::filesystem::absolute(target);
            return std::filesystem::absolute(baseDir / target);
        }
    }

    SequencerEngine::ProjectResult SequencerEngineImpl::loadProject(const std::filesystem::path& projectFile) {
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

        // Clear all existing tracks
        while (!tracks_.empty())
            removeTrack(static_cast<size_t>(0));

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
                    resolvedState = makeAbsolutePathEngine(projectDir, stateFile);

                pending_plugins.fetch_add(1, std::memory_order_relaxed);
                addPluginToTrack(trackIndex, format, pluginId, [this, resolvedState, &pending_plugins](int32_t instanceId, int32_t, std::string error) {
                    if (error.empty() && instanceId >= 0 && !resolvedState.empty()) {
                        auto* instance = getPluginInstance(instanceId);
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
            int32_t trackIndex = addEmptyTrack();
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
                    resolvedPath = makeAbsolutePathEngine(projectDir, resolvedPath);

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
                auto resolvedPath = makeAbsolutePathEngine(projectDir, clip->file());
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

    // ---------------------------------------------------------------------------
    // Master track snapshot (tempo / time-signature map)
    // ---------------------------------------------------------------------------

    SequencerEngine::MasterTrackSnapshot SequencerEngineImpl::buildMasterTrackSnapshot() {
        MasterTrackSnapshot snapshot;
        const double sr = std::max(1.0, static_cast<double>(sampleRate));

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

}
