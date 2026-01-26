#include "uapmd/uapmd.hpp"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <umppi/umppi.hpp>
#include <unordered_set>

#include "../node-graph/UapmdNodeUmpMapper.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {

    int32_t instanceIdSerial{0};

    class SequencerEngineImpl : public SequencerEngine {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        uint32_t default_input_channels_{2};
        uint32_t default_output_channels_{2};
        std::vector<std::unique_ptr<AudioPluginTrack>> tracks_{};
        SequenceProcessContext sequence{};
        int32_t sampleRate;
        AudioPluginHostingAPI* pal;

        // Playback state (managed by AudioPluginSequencer)
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

        // Function block routing
        struct FunctionBlockRoute {
            AudioPluginTrack* track{nullptr};
            int32_t trackIndex{-1};
        };
        std::unordered_map<int32_t, FunctionBlockRoute> plugin_function_blocks_;

        // UMP group allocation
        mutable std::unordered_map<int32_t, uint8_t> plugin_groups_;
        mutable std::unordered_map<uint8_t, int32_t> group_to_instance_;
        std::vector<uint8_t> free_groups_;
        uint8_t next_group_{0};

        // UMP output processing
        std::vector<uapmd_ump_t> plugin_output_scratch_;

        // Plugin instance management
        std::unordered_map<int32_t, AudioPluginInstanceAPI*> plugin_instances_;
        std::unordered_map<int32_t, bool> plugin_bypassed_;
        std::mutex instance_map_mutex_;

        // Parameter listening
        std::unordered_map<int32_t, std::vector<ParameterUpdate>> pending_parameter_updates_;
        std::unordered_set<int32_t> pending_metadata_refresh_;
        std::mutex pending_parameter_mutex_;
        std::unordered_map<int32_t, remidy::EventListenerId> parameter_listener_tokens_;
        std::unordered_map<int32_t, remidy::EventListenerId> metadata_listener_tokens_;
        std::mutex parameter_listener_mutex_;

        // Offline rendering mode
        std::atomic<bool> offline_rendering_{false};

    public:
        explicit SequencerEngineImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts);

        void performPluginScanning(bool rescan) override;

        PluginCatalog& catalog() override;
        std::string getPluginName(int32_t instanceId) override;

        SequenceProcessContext& data() override { return sequence; }

        std::vector<AudioPluginTrack*>& tracks() const override;
        std::vector<TrackInfo> getTrackInfos() override;

        void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;
        void addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback) override;
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
        bool isPluginBypassed(int32_t instanceId) override;
        void setPluginBypassed(int32_t instanceId, bool bypassed) override;

        // Group queries
        std::optional<uint8_t> groupForInstance(int32_t instanceId) const override;
        std::optional<int32_t> instanceForGroup(uint8_t group) const override;

        // Event routing
        void enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) override;

        // Convenience methods for sending MIDI events
        void sendNoteOn(int32_t instanceId, int32_t note) override;
        void sendNoteOff(int32_t instanceId, int32_t note) override;
        void sendPitchBend(int32_t instanceId, float normalizedValue) override;
        void sendChannelPressure(int32_t instanceId, float pressure) override;

        void setParameterValue(int32_t instanceId, int32_t index, double value) override;

        // Parameter listening
        void registerParameterListener(int32_t instanceId, AudioPluginInstanceAPI* instance) override;
        void unregisterParameterListener(int32_t instanceId) override;
        std::vector<ParameterUpdate> getParameterUpdates(int32_t instanceId) override;
        bool consumeParameterMetadataRefresh(int32_t instanceId) override;

        void setPluginOutputHandler(int32_t instanceId, PluginOutputHandler handler) override;

        void assignMidiDeviceToPlugin(int32_t instanceId, std::shared_ptr<MidiIOFeature> device) override;
        void clearMidiDeviceFromPlugin(int32_t instanceId) override;

        bool offlineRendering() const override;
        void offlineRendering(bool enabled) override;

    private:
        void removeTrack(size_t index);

        // Group management
        uint8_t assignGroup(int32_t instanceId);
        void releaseGroup(int32_t instanceId);
        std::optional<uint8_t> groupForInstanceOptional(int32_t instanceId) const;
        std::optional<int32_t> instanceForGroupOptional(uint8_t group) const;

        // Routing configuration
        void configureTrackRouting(AudioPluginTrack* track);
        void refreshFunctionBlockMappings();

        // Route resolution
        struct RouteResolution {
            AudioPluginTrack* track{nullptr};
            int32_t trackIndex{-1};
            int32_t instanceId{-1};
        };
        std::optional<RouteResolution> resolveTarget(int32_t trackOrInstanceId);

        // Output dispatch
        void dispatchPluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes);

        // Application-specific MIDI device integration
        AudioPluginNode* findPluginNodeByInstance(int32_t instanceId);
    };

    std::unique_ptr<SequencerEngine> SequencerEngine::create(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts) {
        return std::make_unique<SequencerEngineImpl>(sampleRate, audioBufferSizeInFrames, umpBufferSizeInInts);
    }

    // SequencerEngineImpl
    SequencerEngineImpl::SequencerEngineImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts) :
        audio_buffer_size_in_frames(audioBufferSizeInFrames),
        sampleRate(sampleRate),
        ump_buffer_size_in_ints(umpBufferSizeInInts),
        pal(AudioPluginHostingAPI::instance()),
        plugin_output_scratch_(umpBufferSizeInInts, 0) {
    }

    std::vector<AudioPluginTrack*> &SequencerEngineImpl::tracks() const {
        // Note: This requires a mutable cache for const correctness
        // Since we need to return a reference to a vector of raw pointers
        static thread_local std::vector<AudioPluginTrack*> track_ptrs;
        track_ptrs.clear();
        for (const auto& track : tracks_)
            track_ptrs.push_back(track.get());
        return track_ptrs;
    }

    int32_t SequencerEngineImpl::processAudio(AudioProcessContext& process) {
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
            auto& tp = *sequence.tracks[i];
            tracks_[i]->processAudio(tp);
            tp.eventIn().position(0); // reset
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

        // FIXME: define status codes
        return 0;
    }

    void SequencerEngineImpl::setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) {
        default_input_channels_ = inputChannels;
        default_output_channels_ = outputChannels;
    }

    void SequencerEngineImpl::addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback) {
        pal->createPluginInstance(sampleRate, default_input_channels_, default_output_channels_, false, format, pluginId, [this,callback](auto instance, std::string error) {
            if (!instance) {
                callback(-1, -1, "Could not create simple track: " + error);
                return;
            }

            // Create track and add plugin
            {
                auto tr = AudioPluginTrack::create(ump_buffer_size_in_ints);
                auto inputMapper = std::make_unique<UapmdNodeUmpInputMapper>(instance.get());
                auto node = std::make_unique<AudioPluginNode>(std::move(instance), instanceIdSerial++);
                tr->graph().appendNodeSimple(std::move(node));
                tracks_.emplace_back(std::move(tr));
                sequence.tracks.emplace_back(new AudioProcessContext(sequence.masterContext(), ump_buffer_size_in_ints));
            }
            auto* track = tracks_.back().get();
            auto trackIndex = static_cast<int32_t>(tracks_.size() - 1);

            // Configure main bus (moved from AudioPluginSequencer)
            auto trackCtx = sequence.tracks[trackIndex];
            trackCtx->configureMainBus(default_input_channels_, default_output_channels_, audio_buffer_size_in_frames);

            // Get the plugin node
            auto plugins = track->graph().plugins();
            if (plugins.empty()) {
                callback(-1, trackIndex, "Track has no plugins after instantiation");
                return;
            }

            auto* pluginNode = plugins.front();
            auto instanceId = pluginNode->instanceId();
            auto* palPtr = pluginNode->pal();

            pluginNode->setUmpInputMapper(std::make_unique<UapmdNodeUmpInputMapper>(palPtr));

            // Function block setup
            configureTrackRouting(track);
            plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, trackIndex};
            assignGroup(instanceId);

            // Plugin instance management
            {
                std::lock_guard<std::mutex> lock(instance_map_mutex_);
                plugin_instances_[instanceId] = palPtr;
                plugin_bypassed_[instanceId] = false;
            }

            // Parameter listening
            registerParameterListener(instanceId, palPtr);

            refreshFunctionBlockMappings();

            track->bypassed(false);
            callback(instanceId, trackIndex, "");
        });
    }

    void SequencerEngineImpl::addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback) {
        // Validate track index
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
            callback(-1, -1, std::format("Invalid track index {}", trackIndex));
            return;
        }

        pal->createPluginInstance(sampleRate, default_input_channels_, default_output_channels_, false, format, pluginId, [this, trackIndex, callback](auto instance, std::string error) {
            if (!instance) {
                callback(-1, -1, "Could not create plugin: " + error);
                return;
            }

            // Re-validate track (may have been removed during async operation)
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
                callback(-1, -1, std::format("Track {} no longer exists", trackIndex));
                return;
            }

            auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
            auto node = std::make_unique<AudioPluginNode>(std::move(instance), instanceIdSerial++);
            auto* nodePtr = node.get();

            // Append to track's graph
            auto status = track->graph().appendNodeSimple(std::move(node));
            if (status != 0) {
                callback(-1, -1, std::format("Failed to append plugin to track {} (status {})", trackIndex, status));
                return;
            }

            auto instanceId = nodePtr->instanceId();
            auto* palPtr = nodePtr->pal();

            nodePtr->setUmpInputMapper(std::make_unique<UapmdNodeUmpInputMapper>(palPtr));

            // Function block setup
            configureTrackRouting(track);
            plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, trackIndex};
            assignGroup(instanceId);

            // Plugin instance management
            {
                std::lock_guard<std::mutex> lock(instance_map_mutex_);
                plugin_instances_[instanceId] = palPtr;
                plugin_bypassed_[instanceId] = false;
            }

            // Parameter listening
            registerParameterListener(instanceId, palPtr);

            refreshFunctionBlockMappings();

            callback(instanceId, trackIndex, "");
        });
    }

    void uapmd::SequencerEngineImpl::setPluginOutputHandler(int32_t instanceId, PluginOutputHandler handler) {
        // FIXME: implement
    }

    bool SequencerEngineImpl::removePluginInstance(int32_t instanceId) {
        // Hide and destroy UI first (if caller didn't already)
        auto* instance = getPluginInstance(instanceId);
        if (instance) {
            if (instance->hasUISupport() && instance->isUIVisible())
                instance->hideUI();
            instance->destroyUI();
        }

        // Parameter listening cleanup
        unregisterParameterListener(instanceId);

        // Clear plugin output handler (application-specific concern)
        setPluginOutputHandler(instanceId, nullptr);

        // Function block cleanup
        plugin_function_blocks_.erase(instanceId);
        releaseGroup(instanceId);

        // Plugin instance cleanup
        {
            std::lock_guard<std::mutex> lock(instance_map_mutex_);
            plugin_instances_.erase(instanceId);
            plugin_bypassed_.erase(instanceId);
        }

        // Remove from track graph
        for (size_t i = 0; i < tracks_.size(); ++i) {
            auto& track = tracks_[i];
            if (!track)
                continue;
            if (track->graph().removePluginInstance(instanceId)) {
                if (track->graph().plugins().empty())
                    removeTrack(i);
                refreshFunctionBlockMappings();
                return true;
            }
        }
        return false;
    }

    void SequencerEngineImpl::removeTrack(size_t index) {
        if (index >= tracks_.size())
            return;
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        if (index < sequence.tracks.size()) {
            delete sequence.tracks[index];
            sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
        }
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

    // Group management
    uint8_t SequencerEngineImpl::assignGroup(int32_t instanceId) {
        auto it = plugin_groups_.find(instanceId);
        if (it != plugin_groups_.end())
            return it->second;

        uint8_t group = 0xFF;
        if (!free_groups_.empty()) {
            group = free_groups_.back();
            free_groups_.pop_back();
        } else {
            if (next_group_ <= 0x0F) {
                group = next_group_;
                ++next_group_;
            } else {
                remidy::Logger::global()->logError("No available UMP groups for plugin instance {}", instanceId);
                group = 0xFF;
            }
        }
        if (group != 0xFF) {
            plugin_groups_[instanceId] = group;
            group_to_instance_[group] = instanceId;
        }
        return group;
    }

    void SequencerEngineImpl::releaseGroup(int32_t instanceId) {
        auto it = plugin_groups_.find(instanceId);
        if (it == plugin_groups_.end())
            return;
        auto group = it->second;
        plugin_groups_.erase(it);
        if (group != 0xFF) {
            group_to_instance_.erase(group);
            if (group <= 0x0F)
                free_groups_.push_back(group);
        }
    }

    std::optional<uint8_t> SequencerEngineImpl::groupForInstanceOptional(int32_t instanceId) const {
        auto it = plugin_groups_.find(instanceId);
        if (it == plugin_groups_.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<int32_t> SequencerEngineImpl::instanceForGroupOptional(uint8_t group) const {
        auto it = group_to_instance_.find(group);
        if (it == group_to_instance_.end())
            return std::nullopt;
        return it->second;
    }

    // Track routing configuration
    void SequencerEngineImpl::configureTrackRouting(AudioPluginTrack* track) {
        if (!track)
            return;
        track->setGroupResolver([this](int32_t instanceId) {
            auto group = groupForInstanceOptional(instanceId);
            return group.has_value() ? group.value() : static_cast<uint8_t>(0xFF);
        });
        track->setEventOutputCallback([this](int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
            dispatchPluginOutput(instanceId, data, bytes);
        });
    }

    void SequencerEngineImpl::refreshFunctionBlockMappings() {
        plugin_function_blocks_.clear();
        auto& trackPtrs = tracks();
        for (size_t trackIndex = 0; trackIndex < trackPtrs.size(); ++trackIndex) {
            auto* track = trackPtrs[trackIndex];
            if (!track)
                continue;
            configureTrackRouting(track);
            auto plugins = track->graph().plugins();
            for (auto* plugin : plugins) {
                if (!plugin)
                    continue;
                auto instanceId = plugin->instanceId();
                plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, static_cast<int32_t>(trackIndex)};
                assignGroup(instanceId);
            }
        }
    }

    std::optional<SequencerEngineImpl::RouteResolution> SequencerEngineImpl::resolveTarget(int32_t trackOrInstanceId) {
        auto instanceIt = plugin_function_blocks_.find(trackOrInstanceId);
        if (instanceIt != plugin_function_blocks_.end()) {
            return RouteResolution{instanceIt->second.track, instanceIt->second.trackIndex, trackOrInstanceId};
        }

        if (auto instFromGroup = instanceForGroupOptional(static_cast<uint8_t>(trackOrInstanceId)); instFromGroup.has_value()) {
            auto mapping = plugin_function_blocks_.find(instFromGroup.value());
            if (mapping != plugin_function_blocks_.end()) {
                return RouteResolution{mapping->second.track, mapping->second.trackIndex, instFromGroup.value()};
            }
        }

        if (trackOrInstanceId < 0)
            return std::nullopt;

        auto& trackPtrs = tracks();
        if (static_cast<size_t>(trackOrInstanceId) >= trackPtrs.size())
            return std::nullopt;

        auto* track = trackPtrs[static_cast<size_t>(trackOrInstanceId)];
        if (!track)
            return std::nullopt;

        configureTrackRouting(track);
        auto plugins = track->graph().plugins();
        if (plugins.empty())
            return std::nullopt;

        auto* first = plugins.front();
        if (!first)
            return std::nullopt;

        auto instanceId = first->instanceId();
        plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, static_cast<int32_t>(trackOrInstanceId)};
        assignGroup(instanceId);
        return RouteResolution{track, static_cast<int32_t>(trackOrInstanceId), instanceId};
    }

    // Plugin output dispatch (with group rewriting + NRPN parameter extraction)
    void SequencerEngineImpl::dispatchPluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
        if (!data || bytes == 0)
            return;

        auto groupOpt = groupForInstanceOptional(instanceId);
        if (!groupOpt.has_value())
            return;
        auto group = groupOpt.value();

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

                // Store parameter update
                {
                    std::lock_guard<std::mutex> parameterLock(pending_parameter_mutex_);
                    pending_parameter_updates_[instanceId].push_back({paramId, value});
                }
            }

            // Rewrite group field
            words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group) << 24);
            offset += size;
        }
    }

    // Parameter listening
    void SequencerEngineImpl::registerParameterListener(int32_t instanceId, AudioPluginInstanceAPI* node) {
        if (!node)
            return;
        auto token = node->addParameterChangeListener(
            [this, instanceId](uint32_t paramIndex, double plainValue) {
                std::lock_guard<std::mutex> lock(pending_parameter_mutex_);
                pending_parameter_updates_[instanceId].push_back({static_cast<int32_t>(paramIndex), plainValue});
            });
        auto metadataToken = node->addParameterMetadataChangeListener([this, instanceId]() {
            std::lock_guard<std::mutex> lock(pending_parameter_mutex_);
            pending_metadata_refresh_.insert(instanceId);
        });
        std::lock_guard<std::mutex> lock(parameter_listener_mutex_);
        if (token != 0)
            parameter_listener_tokens_[instanceId] = token;
        if (metadataToken != 0)
            metadata_listener_tokens_[instanceId] = metadataToken;
    }

    void SequencerEngineImpl::unregisterParameterListener(int32_t instanceId) {
        remidy::EventListenerId token{0};
        remidy::EventListenerId metadataToken{0};
        {
            std::lock_guard<std::mutex> lock(parameter_listener_mutex_);
            auto it = parameter_listener_tokens_.find(instanceId);
            if (it != parameter_listener_tokens_.end()) {
                token = it->second;
                parameter_listener_tokens_.erase(it);
            }
            auto metaIt = metadata_listener_tokens_.find(instanceId);
            if (metaIt != metadata_listener_tokens_.end()) {
                metadataToken = metaIt->second;
                metadata_listener_tokens_.erase(metaIt);
            }
        }
        auto* node = getPluginInstance(instanceId);
        if (node) {
            if (token != 0)
                node->removeParameterChangeListener(token);
            if (metadataToken != 0)
                node->removeParameterMetadataChangeListener(metadataToken);
        }
        {
            std::lock_guard<std::mutex> lock(pending_parameter_mutex_);
            pending_metadata_refresh_.erase(instanceId);
        }
    }

    std::vector<ParameterUpdate> SequencerEngineImpl::getParameterUpdates(int32_t instanceId) {
        std::lock_guard<std::mutex> lock(pending_parameter_mutex_);
        auto it = pending_parameter_updates_.find(instanceId);
        if (it == pending_parameter_updates_.end())
            return {};
        auto updates = std::move(it->second);
        pending_parameter_updates_.erase(it);
        return updates;
    }

    bool SequencerEngineImpl::consumeParameterMetadataRefresh(int32_t instanceId) {
        std::lock_guard<std::mutex> lock(pending_parameter_mutex_);
        auto it = pending_metadata_refresh_.find(instanceId);
        if (it == pending_metadata_refresh_.end())
            return false;
        pending_metadata_refresh_.erase(it);
        return true;
    }

    // Plugin instance queries
    AudioPluginInstanceAPI* SequencerEngineImpl::getPluginInstance(int32_t instanceId) {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        auto it = plugin_instances_.find(instanceId);
        if (it != plugin_instances_.end())
            return it->second;
        return nullptr;
    }

    bool SequencerEngineImpl::isPluginBypassed(int32_t instanceId) {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        auto it = plugin_bypassed_.find(instanceId);
        if (it != plugin_bypassed_.end())
            return it->second;
        return false;
    }

    void SequencerEngineImpl::setPluginBypassed(int32_t instanceId, bool bypassed) {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        plugin_bypassed_[instanceId] = bypassed;
    }

    // Group queries (public API)
    std::optional<uint8_t> SequencerEngineImpl::groupForInstance(int32_t instanceId) const {
        return groupForInstanceOptional(instanceId);
    }

    std::optional<int32_t> SequencerEngineImpl::instanceForGroup(uint8_t group) const {
        return instanceForGroupOptional(group);
    }

    // UMP routing
    void SequencerEngineImpl::enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        auto mapping = plugin_function_blocks_.find(instanceId);
        if (mapping == plugin_function_blocks_.end() || !mapping->second.track)
            return;

        // FIXME: this Function Block mapping is wrong.
        //  We define function blocks per track basis i.e. each track should form a virtual UMP device.
        //  (and also, we currently do not even form one virtual device to gather all the plugins within the track so
        //  every single plugin creates a virtual UMP device exposed to the platform API.)
        //  So mapping will not simply work. We should set group 0 everywhere for now.
        //
        //  However, simply disabling this code causes regression that track > 0 will not receive inputs.
        //  So it is kept for a time being.
        if (auto group = groupForInstanceOptional(instanceId); group.has_value()) {
            auto* bytesPtr = reinterpret_cast<uint8_t*>(ump);
            size_t offset = 0;
            while (offset + sizeof(uint32_t) <= sizeInBytes) {
                auto* words = reinterpret_cast<uint32_t*>(bytesPtr + offset);
                auto messageType = static_cast<uint8_t>(words[0] >> 28);
                auto wordCount = umppi::umpSizeInInts(messageType);
                auto messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
                if (offset + messageSize > sizeInBytes)
                    break;
                words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group.value()) << 24);
                offset += messageSize;
            }
        }

        mapping->second.track->scheduleEvents(timestamp, ump, sizeInBytes);
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

    uapmd::PluginCatalog& uapmd::SequencerEngineImpl::catalog() {
        return pal->catalog();
    }

    void uapmd::SequencerEngineImpl::performPluginScanning(bool rescan) {
        pal->performPluginScanning(rescan);
    }

    std::vector<SequencerEngineImpl::TrackInfo> uapmd::SequencerEngineImpl::getTrackInfos() {
        std::vector<TrackInfo> info;
        auto catalogPlugins = pal->catalog().getPlugins();
        auto displayNameFor = [&](const std::string& format, const std::string& pluginId) -> std::string {
            for (auto* entry : catalogPlugins) {
                if (entry->format() == format && entry->pluginId() == pluginId) {
                    return entry->displayName();
                }
            }
            return pluginId;
        };

        auto& tracksRef = tracks();
        info.reserve(tracksRef.size());
        for (size_t i = 0; i < tracksRef.size(); ++i) {
            TrackInfo trackInfo;
            trackInfo.trackIndex = static_cast<int32_t>(i);
            for (auto* plugin : tracksRef[i]->graph().plugins()) {
                PluginNodeInfo nodeInfo;
                nodeInfo.instanceId = plugin->instanceId();
                nodeInfo.pluginId = plugin->pal()->pluginId();
                nodeInfo.format = plugin->pal()->formatName();
                nodeInfo.displayName = displayNameFor(nodeInfo.format, nodeInfo.pluginId);
                trackInfo.nodes.push_back(std::move(nodeInfo));
            }
            info.push_back(std::move(trackInfo));
        }
        return info;
    }

    std::string uapmd::SequencerEngineImpl::getPluginName(int32_t instanceId) {
        auto* instance = getPluginInstance(instanceId);
        // Get plugin ID and look it up in the catalog
        std::string pluginId = instance->pluginId();
        std::string format = instance->formatName();

        // Search in the catalog for display name
        auto plugins = pal->catalog().getPlugins();
        for (auto* catalogPlugin : plugins) {
            if (catalogPlugin->pluginId() == pluginId && catalogPlugin->format() == format)
                return catalogPlugin->displayName();
        }
        return "Plugin " + std::to_string(instanceId);
    }

    uapmd::AudioPluginNode* uapmd::SequencerEngineImpl::findPluginNodeByInstance(int32_t instanceId) {
        auto& tracksRef = tracks();
        for (auto* track : tracksRef) {
            if (!track)
                continue;
            for (auto* plugin : track->graph().plugins()) {
                if (plugin && plugin->instanceId() == instanceId)
                    return plugin;
            }
        }
        return nullptr;
    }

    void uapmd::SequencerEngineImpl::assignMidiDeviceToPlugin(int32_t instanceId, std::shared_ptr<MidiIOFeature> device) {
        if (!device)
            return;
        auto* node = findPluginNodeByInstance(instanceId);
        if (!node)
            return;
        auto* palPtr = node->pal();
        if (!palPtr)
            return;
        auto mapper = std::make_unique<UapmdNodeUmpOutputMapper>(std::move(device), palPtr);
        node->setUmpOutputMapper(std::move(mapper));
    }

    void uapmd::SequencerEngineImpl::clearMidiDeviceFromPlugin(int32_t instanceId) {
        auto* node = findPluginNodeByInstance(instanceId);
        if (!node)
            return;
        node->setUmpOutputMapper(nullptr);
    }

    bool uapmd::SequencerEngineImpl::offlineRendering() const {
        return offline_rendering_.load(std::memory_order_acquire);
    }

    void uapmd::SequencerEngineImpl::offlineRendering(bool enabled) {
        offline_rendering_.store(enabled, std::memory_order_release);
    }

}
