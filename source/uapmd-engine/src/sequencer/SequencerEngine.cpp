#include "uapmd/uapmd.hpp"
#include <atomic>
#include <format>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <umppi/umppi.hpp>

#include <remidy/remidy.hpp>
#include "uapmd-engine/uapmd-engine.hpp"
#include "readerwriterqueue.h"

namespace uapmd {

    // ── Pump / RT ring-buffer structures ─────────────────────────────────────
    //
    // Layer 1 (pump) pre-fills AudioProcessContext input buffers one quantum at
    // a time and enqueues the slot index to the RT consumer via `filled`.  The RT
    // layer (Layer 2) dequeues a slot, runs AudioPluginGraph::processAudio() using
    // that slot's context, mixes the outputs, and returns the slot to `free_slots`.
    //
    // kPumpLookahead is the maximum number of quanta the pump can run ahead of the
    // RT thread.  kPumpSlots = kPumpLookahead + 1 ensures the pump always has at
    // least one writable slot while the RT thread holds one readable slot.

    static constexpr size_t kPumpLookahead = 4;
    static constexpr size_t kPumpSlots     = kPumpLookahead + 1;

    struct PumpSlot {
        std::unique_ptr<AudioProcessContext> ctx;
    };

    struct PumpTrackRing {
        std::array<PumpSlot, kPumpSlots> slots;
        moodycamel::ReaderWriterQueue<size_t> filled{kPumpSlots};
        moodycamel::ReaderWriterQueue<size_t> free_slots{kPumpSlots};

        explicit PumpTrackRing(MasterContext& mc, size_t umpBufSizeInInts) {
            for (size_t i = 0; i < kPumpSlots; i++) {
                slots[i].ctx = std::make_unique<AudioProcessContext>(mc, umpBufSizeInInts);
                free_slots.try_enqueue(i);
            }
        }
    };

    struct OutputAlignmentDelayLine {
        std::vector<std::vector<std::vector<float>>> buses;
        size_t write_position{0};
        size_t capacity_frames{0};

        void reset() {
            write_position = 0;
            for (auto& bus : buses)
                for (auto& channel : bus)
                    std::fill(channel.begin(), channel.end(), 0.0f);
        }

        void configure(AudioProcessContext* ctx, size_t delayFrames) {
            capacity_frames = std::max<size_t>(1, delayFrames + 1);
            const uint32_t busCount = ctx ? ctx->audioOutBusCount() : 0;
            buses.assign(busCount, {});
            for (uint32_t busIndex = 0; busIndex < busCount; ++busIndex) {
                const uint32_t channelCount = ctx->outputChannelCount(busIndex);
                buses[busIndex].assign(channelCount, std::vector<float>(capacity_frames, 0.0f));
            }
            write_position = 0;
        }
    };

    static void clearAudioInputBuses(AudioProcessContext& ctx) {
        for (int32_t busIndex = 0; busIndex < ctx.audioInBusCount(); ++busIndex)
            for (uint32_t ch = 0; ch < ctx.inputChannelCount(busIndex); ++ch) {
                auto* buffer = ctx.getFloatInBuffer(busIndex, ch);
                if (buffer)
                    std::memset(buffer, 0, static_cast<size_t>(ctx.frameCount()) * sizeof(float));
            }
    }

    static void accumulateAudioBus(
        AudioProcessContext& dstCtx,
        uint32_t dstBusIndex,
        const AudioProcessContext& srcCtx,
        uint32_t srcBusIndex,
        int32_t frameCount) {
        if (dstBusIndex >= static_cast<uint32_t>(dstCtx.audioOutBusCount()) ||
            srcBusIndex >= static_cast<uint32_t>(srcCtx.audioOutBusCount()))
            return;
        const uint32_t numChannels = std::min(
            static_cast<uint32_t>(dstCtx.outputChannelCount(static_cast<int32_t>(dstBusIndex))),
            static_cast<uint32_t>(srcCtx.outputChannelCount(static_cast<int32_t>(srcBusIndex))));
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            auto* dst = dstCtx.getFloatOutBuffer(static_cast<int32_t>(dstBusIndex), ch);
            const auto* src = srcCtx.getFloatOutBuffer(static_cast<int32_t>(srcBusIndex), ch);
            if (!dst || !src)
                continue;
            for (int32_t frame = 0; frame < frameCount; ++frame)
                dst[frame] += src[frame];
        }
    }

    static void accumulateAudioBusToInput(
        AudioProcessContext& dstCtx,
        uint32_t dstBusIndex,
        const AudioProcessContext& srcCtx,
        uint32_t srcBusIndex,
        int32_t frameCount) {
        if (dstBusIndex >= static_cast<uint32_t>(dstCtx.audioInBusCount()) ||
            srcBusIndex >= static_cast<uint32_t>(srcCtx.audioOutBusCount()))
            return;
        const uint32_t numChannels = std::min(
            static_cast<uint32_t>(dstCtx.inputChannelCount(static_cast<int32_t>(dstBusIndex))),
            static_cast<uint32_t>(srcCtx.outputChannelCount(static_cast<int32_t>(srcBusIndex))));
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            auto* dst = dstCtx.getFloatInBuffer(static_cast<int32_t>(dstBusIndex), ch);
            const auto* src = srcCtx.getFloatOutBuffer(static_cast<int32_t>(srcBusIndex), ch);
            if (!dst || !src)
                continue;
            for (int32_t frame = 0; frame < frameCount; ++frame)
                dst[frame] += src[frame];
        }
    }

    static void applyTrackBusesLayout(SequencerTrack* track, const AudioGraphBusesLayout& layout) {
        if (!track)
            return;
        auto* extension = track->graph().getExtension<AudioBusesLayoutExtension>();
        if (!extension)
            return;
        extension->applyBusesLayout(layout);
    }

    // ─────────────────────────────────────────────────────────────────────────

    class SequencerEngineImpl : public SequencerEngine {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        uint32_t default_input_channels_{2};
        uint32_t default_output_channels_{2};
        std::vector<std::unique_ptr<SequencerTrack>> tracks_{};
        std::unique_ptr<SequencerTrack> master_track_;
        std::unique_ptr<AudioProcessContext> master_track_context_;
        std::unique_ptr<AudioProcessContext> mix_bus_context_;
        SequenceProcessContext sequence{};
        int32_t sampleRate;
        std::unique_ptr<AudioPluginHostingAPI> plugin_host;
        UapmdFunctionBlockManager function_block_manager{};
        std::atomic<OutputAlignmentMonitoringPolicy> output_alignment_monitoring_policy_{
            OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT};
        std::atomic<RealtimeInfiniteTailPolicy> realtime_infinite_tail_policy_{
            RealtimeInfiniteTailPolicy::LATENCY_FALLBACK};

        // Playback state (managed by RealtimeSequencer)
        std::atomic<bool> is_playback_active_{false};
        std::atomic<int64_t> playback_position_samples_{0};
        std::atomic<int64_t> render_playback_position_samples_{0};
        std::atomic<bool> latency_drain_active_{false};
        std::atomic<int64_t> latency_drain_remaining_samples_{0};
        std::atomic<bool> reset_to_start_after_latency_drain_{false};

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


        // When true, processAudio() skips the inline pumpAudio() call because a
        // dedicated pump pthread (WebAudioEngineThread) is driving it independently.
        std::atomic<bool> external_pump_{false};

        // Offline rendering mode
        std::atomic<bool> offline_rendering_{false};
        TrackOutputHandler track_output_handler_{};

        // Engine active flag: when false, processAudio outputs silence without invoking plugins.
        // Starts inactive so that no plugin code runs before the user explicitly enables the
        // audio engine (important on Emscripten where AudioWorklet fires immediately after
        // connect before lazy-initialized statics are guaranteed to be ready).
        std::atomic<bool> engine_active_{false};

        // Track processing flags for safe deletion (parallel to tracks_ vector)
        // Note: std::atomic is not copyable, so we use unique_ptr
        std::vector<std::unique_ptr<std::atomic<bool>>> track_processing_flags_;

        // Pump / RT ring-buffer state.  pump_rings_[t] is the per-track ring;
        // pump_sequence_.tracks[t] is a non-owning pointer that the pump temporarily
        // redirects to whichever ring slot it is currently filling.
        std::vector<std::unique_ptr<PumpTrackRing>> pump_rings_;
        SequenceProcessContext pump_sequence_{};
        // Pre-allocated work vectors — kept in sync with tracks_.size() so the
        // hot paths never allocate.
        std::vector<size_t> pump_slot_indices_;   // pump thread: slot acquired per track
        std::vector<size_t> rt_dequeued_slots_;   // RT thread: slot dequeued per track
        std::vector<OutputAlignmentDelayLine> output_alignment_delay_lines_;

        void ensureTrackBusConfiguration(int32_t trackIndex, remidy::PluginAudioBuses* pluginBuses);
        void ensureContextBusConfiguration(AudioProcessContext* ctx, remidy::PluginAudioBuses* pluginBuses);
        std::vector<remidy::AudioBusSpec> mergeBusSpecs(const std::vector<remidy::AudioBusSpec>& current,
                                                        const std::vector<remidy::AudioBusConfiguration*>& pluginBuses);

        // Timeline facade (owns timeline tracks, clips, project loading)
        std::unique_ptr<TimelineFacade> timeline_;

    public:
        explicit SequencerEngineImpl(
            int32_t sampleRate,
            size_t audioBufferSizeInFrames,
            size_t umpBufferSizeInInts);
        ~SequencerEngineImpl() override;

        AudioPluginHostingAPI* pluginHost() override;

        SequenceProcessContext& data() override { return sequence; }

        std::vector<SequencerTrack*>& tracks() const override;
        SequencerTrack* masterTrack() override;
        size_t umpBufferSizeInBytes() const override { return ump_buffer_size_in_ints; }
        uint32_t trackLatencyInSamples(uapmd_track_index_t trackIndex) override;
        uint32_t masterTrackLatencyInSamples() override;
        uint32_t trackRenderLeadInSamples(uapmd_track_index_t trackIndex) override;
        uint32_t masterTrackRenderLeadInSamples() override;
        bool trackHasLiveInput(uapmd_track_index_t trackIndex) override;
        uint32_t trackOutputAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex) override;
        uint32_t trackOutputBusAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) override;
        TrackOutputRoutingTarget trackOutputBusRoutingTarget(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) override;
        bool isOutputAlignmentActive() override;
        OutputAlignmentMonitoringPolicy outputAlignmentMonitoringPolicy() const override;
        void outputAlignmentMonitoringPolicy(OutputAlignmentMonitoringPolicy policy) override;
        RealtimeInfiniteTailPolicy realtimeInfiniteTailPolicy() const override;
        void realtimeInfiniteTailPolicy(RealtimeInfiniteTailPolicy policy) override;

        void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;
        uapmd_track_index_t addEmptyTrack() override;
        bool removeTrack(uapmd_track_index_t trackIndex) override;
        bool replaceTrackGraph(uapmd_track_index_t trackIndex, std::unique_ptr<AudioPluginGraph>&& graph) override;
        void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback) override;
        bool removePluginInstance(int32_t instanceId) override;

        uint8_t getInstanceGroup(int32_t instanceId) const override {
            for (const auto& t : tracks_)
                if (t) {
                    auto g = t->getInstanceGroup(instanceId);
                    if (g != 0xFFu) return g;
                }
            if (master_track_)
                return master_track_->getInstanceGroup(instanceId);
            return 0xFFu;
        }

        bool setInstanceGroup(int32_t instanceId, uint8_t group) override {
            // Find which track owns this instance and set the group there.
            auto setOnTrack = [&](SequencerTrack* t) -> bool {
                if (!t) return false;
                for (int32_t id : t->orderedInstanceIds()) {
                    if (id != instanceId) continue;
                    // Check for conflicts (another instance already using this group).
                    for (int32_t otherId : t->orderedInstanceIds())
                        if (otherId != instanceId && t->getInstanceGroup(otherId) == group)
                            return false; // conflict
                    t->setInstanceGroup(instanceId, group);
                    return true;
                }
                return false;
            };
            for (const auto& t : tracks_)
                if (setOnTrack(t.get())) return true;
            return setOnTrack(master_track_.get());
        }

        void setAudioPreprocessCallback(AudioPreprocessCallback callback) override {
            audio_preprocess_callback_ = std::move(callback);
        }

        void setExternalPump(bool enabled) override {
            external_pump_.store(enabled, std::memory_order_release);
        }
        void setTrackOutputHandler(TrackOutputHandler handler) override {
            track_output_handler_ = std::move(handler);
        }

        void pumpAudio(AudioProcessContext& process) override;
        uapmd_status_t processAudio(AudioProcessContext& process) override;

        // Playback control
        bool isPlaybackActive() const override;
        void playbackPosition(int64_t samples) override;
        int64_t playbackPosition() const override;
        int64_t renderPlaybackPosition() const override;
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

        void setEngineActive(bool active) override {
            engine_active_.store(active, std::memory_order_release);
        }

        void cleanupEmptyTracks() override;

        // Timeline facade
        TimelineFacade& timeline() override { return *timeline_; }

    private:
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
        void schedulePrerollFromAudiblePosition(int64_t samples);
        uint32_t maxTrackRenderLeadInSamples() const;
        uint32_t maxRenderLeadInSamples() const;
        uint32_t maxLiveInputRenderLeadInSamples() const;
        uint32_t maxOutputAlignmentHoldbackInSamples() const;
        uint32_t trackOutputAlignmentHoldbackInSamplesImpl(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) const;
        TrackOutputRoutingTarget trackOutputBusRoutingTargetImpl(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) const;
        int64_t maxStopDrainInSamples() const;
        double tailLengthSecondsToSamples(double seconds) const;
        void reconfigureMasterTrackInputBuses();
        void updateLatencyDrainState(int32_t frameCount);
        void reconfigureMixBusContext();
        void reconfigureOutputAlignmentBuffers();
        void resetOutputAlignmentBuffers();
    };

    std::unique_ptr<SequencerEngine> SequencerEngine::create(
        int32_t sampleRate,
        size_t audioBufferSizeInFrames,
        size_t umpBufferSizeInInts
    ) {
        return std::make_unique<SequencerEngineImpl>(
            sampleRate,
            audioBufferSizeInFrames,
            umpBufferSizeInInts);
    }

    // SequencerEngineImpl
    SequencerEngineImpl::SequencerEngineImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts) :
        audio_buffer_size_in_frames(audioBufferSizeInFrames),
        sampleRate(sampleRate),
        ump_buffer_size_in_ints(umpBufferSizeInInts),
        plugin_host(AudioPluginHostingAPI::create()),
        plugin_output_scratch_(umpBufferSizeInInts, 0) {
        timeline_ = TimelineFacade::create(*this);
        master_track_ = SequencerTrack::create(
            timeline_->audioGraphProviderRegistry(),
            umpBufferSizeInInts,
            "");
        master_track_context_ = std::make_unique<AudioProcessContext>(sequence.masterContext(), ump_buffer_size_in_ints);
        mix_bus_context_ = std::make_unique<AudioProcessContext>(sequence.masterContext(), ump_buffer_size_in_ints);
        if (master_track_context_) {
            master_track_context_->configureMainBus(default_output_channels_, default_output_channels_, audio_buffer_size_in_frames);
            applyTrackBusesLayout(master_track_.get(), AudioGraphBusesLayout{
                static_cast<uint32_t>(master_track_context_->audioInBusCount()),
                static_cast<uint32_t>(master_track_context_->audioOutBusCount()),
                1,
                1,
            });
        }
        reconfigureMixBusContext();
        configureTrackRouting(master_track_.get());

        // Call the pump-aware overload so that processTracksAudio writes into
        // pump_sequence_.tracks[i] (ring-buffer slots) instead of sequence.tracks[i].
        audio_preprocess_callback_ = [this](AudioProcessContext& process) {
            timeline_->processTracksAudio(process, pump_sequence_);
        };
        reconfigureOutputAlignmentBuffers();
    }

    SequencerEngineImpl::~SequencerEngineImpl() {
        // Detach output mappers while plugin instances are still alive. This is a separate
        // step from clearAllDevices() because AppModel::DeviceState holds shared_ptrs to
        // UapmdFunctionBlock that may outlive the engine — detaching now ensures those
        // delayed destructions won't access freed PluginParameterSupport objects.
        function_block_manager.detachAllOutputMappers();
        function_block_manager.clearAllDevices();
        // Make sure to clean up tracks before plugin_host so that we don't release plugin instances ahead of these.
        tracks_.clear();
    }

    std::vector<remidy::AudioBusSpec> SequencerEngineImpl::mergeBusSpecs(
        const std::vector<remidy::AudioBusSpec>& current,
        const std::vector<remidy::AudioBusConfiguration*>& pluginBuses) {
        auto merged = current;
        for (size_t i = 0; i < pluginBuses.size(); ++i) {
            auto* bus = pluginBuses[i];
            if (!bus || !bus->enabled())
                continue;
            remidy::AudioBusSpec required{
                bus->role(),
                bus->channelLayout().channels(),
                audio_buffer_size_in_frames
            };
            if (i >= merged.size()) {
                merged.emplace_back(required);
            } else {
                merged[i].channels = std::max(merged[i].channels, required.channels);
                merged[i].bufferCapacityFrames = std::max(merged[i].bufferCapacityFrames, required.bufferCapacityFrames);
                if (required.role == remidy::AudioBusRole::Main)
                    merged[i].role = remidy::AudioBusRole::Main;
            }
        }
        return merged;
    }

    void SequencerEngineImpl::ensureContextBusConfiguration(AudioProcessContext* ctx,
                                                            remidy::PluginAudioBuses* pluginBuses) {
        if (!ctx || !pluginBuses)
            return;
        const auto& inputSpecsRef = ctx->audioInputSpecs();
        const auto& outputSpecsRef = ctx->audioOutputSpecs();
        auto currentInput = std::vector<remidy::AudioBusSpec>(inputSpecsRef.begin(), inputSpecsRef.end());
        auto currentOutput = std::vector<remidy::AudioBusSpec>(outputSpecsRef.begin(), outputSpecsRef.end());
        auto mergedInput = mergeBusSpecs(currentInput, pluginBuses->audioInputBuses());
        auto mergedOutput = mergeBusSpecs(currentOutput, pluginBuses->audioOutputBuses());

        if (!mergedInput.empty() && mergedInput != currentInput)
            ctx->configureAudioInputBuses(mergedInput);
        if (!mergedOutput.empty() && mergedOutput != currentOutput)
            ctx->configureAudioOutputBuses(mergedOutput);
    }

    void SequencerEngineImpl::ensureTrackBusConfiguration(int32_t trackIndex,
                                                          remidy::PluginAudioBuses* pluginBuses) {
        if (!pluginBuses)
            return;
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= sequence.tracks.size())
            return;
        auto* ctx = sequence.tracks[static_cast<size_t>(trackIndex)];
        if (!ctx)
            return;

        if (static_cast<size_t>(trackIndex) < track_processing_flags_.size()) {
            auto* processingFlag = track_processing_flags_[static_cast<size_t>(trackIndex)].get();
            if (processingFlag) {
                while (processingFlag->load(std::memory_order_acquire))
                    std::this_thread::yield();
            }
        }

        ensureContextBusConfiguration(ctx, pluginBuses);
        applyTrackBusesLayout(tracks_[static_cast<size_t>(trackIndex)].get(), AudioGraphBusesLayout{
            static_cast<uint32_t>(ctx->audioInBusCount()),
            static_cast<uint32_t>(ctx->audioOutBusCount()),
            1,
            1,
        });

        // Keep pump ring slot contexts in sync so they have the same bus layout.
        if (static_cast<size_t>(trackIndex) < pump_rings_.size())
            for (auto& slot : pump_rings_[static_cast<size_t>(trackIndex)]->slots)
                ensureContextBusConfiguration(slot.ctx.get(), pluginBuses);
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

    uint32_t SequencerEngineImpl::trackLatencyInSamples(uapmd_track_index_t trackIndex) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return 0;
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        return track ? track->latencyInSamples() : 0;
    }

    uint32_t SequencerEngineImpl::masterTrackLatencyInSamples() {
        return master_track_ ? master_track_->latencyInSamples() : 0;
    }

    uint32_t SequencerEngineImpl::trackRenderLeadInSamples(uapmd_track_index_t trackIndex) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return 0;
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        return track ? track->renderLeadInSamples() : 0;
    }

    uint32_t SequencerEngineImpl::masterTrackRenderLeadInSamples() {
        return master_track_ ? master_track_->renderLeadInSamples() : 0;
    }

    bool SequencerEngineImpl::trackHasLiveInput(uapmd_track_index_t trackIndex) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size() || !timeline_)
            return false;
        return timeline_->trackHasLiveInput(trackIndex);
    }

    uint32_t SequencerEngineImpl::trackOutputAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex) {
        return trackOutputAlignmentHoldbackInSamplesImpl(trackIndex, 0);
    }

    uint32_t SequencerEngineImpl::trackOutputBusAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) {
        return trackOutputAlignmentHoldbackInSamplesImpl(trackIndex, outputBusIndex);
    }

    TrackOutputRoutingTarget SequencerEngineImpl::trackOutputBusRoutingTarget(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) {
        return trackOutputBusRoutingTargetImpl(trackIndex, outputBusIndex);
    }

    bool SequencerEngineImpl::isOutputAlignmentActive() {
        if (!timeline_)
            return false;
        for (size_t i = 0; i < tracks_.size(); ++i) {
            auto* track = tracks_[i].get();
            if (!track)
                continue;
            for (uint32_t busIndex = 0; busIndex < track->graph().outputBusCount(); ++busIndex)
                if (trackOutputAlignmentHoldbackInSamplesImpl(static_cast<uapmd_track_index_t>(i), busIndex) > 0)
                    return true;
        }
        return false;
    }

    OutputAlignmentMonitoringPolicy SequencerEngineImpl::outputAlignmentMonitoringPolicy() const {
        return output_alignment_monitoring_policy_.load(std::memory_order_acquire);
    }

    void SequencerEngineImpl::outputAlignmentMonitoringPolicy(OutputAlignmentMonitoringPolicy policy) {
        output_alignment_monitoring_policy_.store(policy, std::memory_order_release);
        resetOutputAlignmentBuffers();
    }

    RealtimeInfiniteTailPolicy SequencerEngineImpl::realtimeInfiniteTailPolicy() const {
        return realtime_infinite_tail_policy_.load(std::memory_order_acquire);
    }

    void SequencerEngineImpl::realtimeInfiniteTailPolicy(RealtimeInfiniteTailPolicy policy) {
        realtime_infinite_tail_policy_.store(policy, std::memory_order_release);
    }

    uint32_t SequencerEngineImpl::maxTrackRenderLeadInSamples() const {
        uint32_t maxTrackLatency = 0;
        for (size_t i = 0; i < tracks_.size(); ++i) {
            auto* track = tracks_[i].get();
            if (!track)
                continue;
            maxTrackLatency = std::max(maxTrackLatency, track->renderLeadInSamples());
        }
        return maxTrackLatency;
    }

    uint32_t SequencerEngineImpl::maxRenderLeadInSamples() const {
        const uint32_t maxTrackLatency = maxTrackRenderLeadInSamples();
        const uint32_t masterLatency = master_track_ ? master_track_->renderLeadInSamples() : 0;
        return maxTrackLatency + masterLatency;
    }

    uint32_t SequencerEngineImpl::maxLiveInputRenderLeadInSamples() const {
        uint32_t maxLead = 0;
        if (!timeline_)
            return 0;
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (!timeline_->trackHasLiveInput(static_cast<int32_t>(i)))
                continue;
            auto* track = tracks_[i].get();
            if (!track)
                continue;
            maxLead = std::max(maxLead, track->renderLeadInSamples());
        }
        return maxLead;
    }

    uint32_t SequencerEngineImpl::maxOutputAlignmentHoldbackInSamples() const {
        uint32_t maxHoldback = 0;
        for (size_t i = 0; i < tracks_.size(); ++i) {
            auto* track = tracks_[i].get();
            if (!track)
                continue;
            for (uint32_t busIndex = 0; busIndex < track->graph().outputBusCount(); ++busIndex)
                maxHoldback = std::max(
                    maxHoldback,
                    trackOutputAlignmentHoldbackInSamplesImpl(static_cast<uapmd_track_index_t>(i), busIndex));
        }
        return maxHoldback;
    }

    uint32_t SequencerEngineImpl::trackOutputAlignmentHoldbackInSamplesImpl(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size() || !timeline_)
            return 0;
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        if (!track)
            return 0;
        if (outputBusIndex >= track->graph().outputBusCount())
            return 0;

        const uint32_t trackLead = track->renderLeadInSamples();
        const uint32_t outputLatency = track->graph().outputLatencyInSamples(outputBusIndex);
        const uint32_t intraTrackHoldback = trackLead > outputLatency ? trackLead - outputLatency : 0;
        if (output_alignment_monitoring_policy_.load(std::memory_order_acquire) !=
            OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT)
            return intraTrackHoldback;

        const uint32_t liveInputReferenceLead = maxLiveInputRenderLeadInSamples();
        if (liveInputReferenceLead == 0)
            return intraTrackHoldback;
        if (timeline_->trackHasLiveInput(trackIndex))
            return 0;
        return liveInputReferenceLead + intraTrackHoldback;
    }

    TrackOutputRoutingTarget SequencerEngineImpl::trackOutputBusRoutingTargetImpl(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return {};
        auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
        if (!track || outputBusIndex >= track->graph().outputBusCount())
            return {};

        if (master_track_ && master_track_context_ && !master_track_->orderedInstanceIds().empty() &&
            master_track_context_->audioInBusCount() > 0) {
            if (outputBusIndex < static_cast<uint32_t>(master_track_context_->audioInBusCount()))
                return {
                    TrackOutputRoutingTargetType::MASTER_INPUT_BUS,
                    outputBusIndex,
                    false,
                };
            return {
                TrackOutputRoutingTargetType::MASTER_INPUT_BUS,
                0,
                true,
            };
        }

        return {
            TrackOutputRoutingTargetType::MAIN_MIX_BUS,
            0,
            outputBusIndex != 0,
        };
    }

    double SequencerEngineImpl::tailLengthSecondsToSamples(double seconds) const {
        if (!(seconds > 0.0))
            return 0.0;
        if (!std::isfinite(seconds))
            return std::numeric_limits<double>::infinity();
        return std::ceil(seconds * static_cast<double>(sampleRate));
    }

    int64_t SequencerEngineImpl::maxStopDrainInSamples() const {
        const double masterPathSamples =
            static_cast<double>(master_track_ ? master_track_->renderLeadInSamples() : 0) +
            tailLengthSecondsToSamples(master_track_ ? master_track_->tailLengthInSeconds() : 0.0);

        double maxTrackPathSamples = 0.0;
        for (const auto& track : tracks_) {
            if (!track)
                continue;
            const double trackPathSamples =
                static_cast<double>(track->renderLeadInSamples()) +
                tailLengthSecondsToSamples(track->tailLengthInSeconds());
            if (!std::isfinite(trackPathSamples) || !std::isfinite(masterPathSamples))
                return realtime_infinite_tail_policy_.load(std::memory_order_acquire) ==
                    RealtimeInfiniteTailPolicy::IMMEDIATE_STOP
                    ? 0
                    : static_cast<int64_t>(maxRenderLeadInSamples());
            maxTrackPathSamples = std::max(maxTrackPathSamples, trackPathSamples);
        }

        const double totalDrainSamples =
            tracks_.empty() ? masterPathSamples : maxTrackPathSamples + masterPathSamples;
        if (!std::isfinite(totalDrainSamples))
            return realtime_infinite_tail_policy_.load(std::memory_order_acquire) ==
                RealtimeInfiniteTailPolicy::IMMEDIATE_STOP
                ? 0
                : static_cast<int64_t>(maxRenderLeadInSamples());
        if (totalDrainSamples <= 0.0)
            return 0;
        if (totalDrainSamples >= static_cast<double>(std::numeric_limits<int64_t>::max()))
            return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(totalDrainSamples);
    }

    void SequencerEngineImpl::reconfigureMasterTrackInputBuses() {
        if (!master_track_context_ || !mix_bus_context_)
            return;

        std::vector<remidy::AudioBusSpec> mergedInputSpecs(
            master_track_context_->audioInputSpecs().begin(),
            master_track_context_->audioInputSpecs().end());
        const auto& mixOutputSpecs = mix_bus_context_->audioOutputSpecs();
        for (size_t busIndex = 0; busIndex < mixOutputSpecs.size(); ++busIndex) {
            const auto& mixSpec = mixOutputSpecs[busIndex];
            if (busIndex >= mergedInputSpecs.size()) {
                mergedInputSpecs.push_back(mixSpec);
                continue;
            }
            mergedInputSpecs[busIndex].channels = std::max(mergedInputSpecs[busIndex].channels, mixSpec.channels);
            mergedInputSpecs[busIndex].bufferCapacityFrames = std::max(
                mergedInputSpecs[busIndex].bufferCapacityFrames,
                mixSpec.bufferCapacityFrames);
            if (mixSpec.role == remidy::AudioBusRole::Main)
                mergedInputSpecs[busIndex].role = remidy::AudioBusRole::Main;
        }

        if (mergedInputSpecs != master_track_context_->audioInputSpecs())
            master_track_context_->configureAudioInputBuses(mergedInputSpecs);
        if (master_track_)
            applyTrackBusesLayout(master_track_.get(), AudioGraphBusesLayout{
                static_cast<uint32_t>(master_track_context_->audioInBusCount()),
                static_cast<uint32_t>(master_track_context_->audioOutBusCount()),
                1,
                1,
            });
    }

    void SequencerEngineImpl::reconfigureMixBusContext() {
        if (!mix_bus_context_)
            return;

        std::vector<remidy::AudioBusSpec> mixSpecs;
        for (const auto* ctx : sequence.tracks) {
            if (!ctx)
                continue;
            const auto& outputSpecs = ctx->audioOutputSpecs();
            for (size_t busIndex = 0; busIndex < outputSpecs.size(); ++busIndex) {
                const auto& spec = outputSpecs[busIndex];
                if (busIndex >= mixSpecs.size()) {
                    mixSpecs.push_back(spec);
                    continue;
                }
                mixSpecs[busIndex].channels = std::max(mixSpecs[busIndex].channels, spec.channels);
                mixSpecs[busIndex].bufferCapacityFrames = std::max(
                    mixSpecs[busIndex].bufferCapacityFrames,
                    spec.bufferCapacityFrames);
                if (spec.role == remidy::AudioBusRole::Main)
                    mixSpecs[busIndex].role = remidy::AudioBusRole::Main;
            }
        }

        if (mixSpecs.empty())
            mixSpecs.push_back(
                remidy::AudioBusSpec{
                    remidy::AudioBusRole::Main,
                    default_output_channels_,
                    audio_buffer_size_in_frames});
        mix_bus_context_->configureAudioOutputBuses(mixSpecs);
        reconfigureMasterTrackInputBuses();
    }

    void SequencerEngineImpl::reconfigureOutputAlignmentBuffers() {
        output_alignment_delay_lines_.resize(tracks_.size());
        const size_t delayCapacityFrames = static_cast<size_t>(maxOutputAlignmentHoldbackInSamples()) + audio_buffer_size_in_frames;
        for (size_t i = 0; i < output_alignment_delay_lines_.size(); ++i) {
            auto* ctx = i < sequence.tracks.size() ? sequence.tracks[i] : nullptr;
            output_alignment_delay_lines_[i].configure(ctx, delayCapacityFrames);
        }
    }

    void SequencerEngineImpl::resetOutputAlignmentBuffers() {
        for (auto& delayLine : output_alignment_delay_lines_)
            delayLine.reset();
    }

    void SequencerEngineImpl::schedulePrerollFromAudiblePosition(int64_t samples) {
        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        reset_to_start_after_latency_drain_.store(false, std::memory_order_release);
        playback_position_samples_.store(samples, std::memory_order_release);
        const auto prerollSamples = static_cast<int64_t>(maxRenderLeadInSamples());
        const auto quantum = static_cast<int64_t>(audio_buffer_size_in_frames > 0 ? audio_buffer_size_in_frames : 1);
        const auto alignedPreroll =
            ((prerollSamples + quantum - 1) / quantum) * quantum;
        render_playback_position_samples_.store(samples - alignedPreroll, std::memory_order_release);
    }

    void SequencerEngineImpl::updateLatencyDrainState(int32_t frameCount) {
        if (!latency_drain_active_.load(std::memory_order_acquire))
            return;

        const auto remaining =
            latency_drain_remaining_samples_.fetch_sub(frameCount, std::memory_order_acq_rel) - frameCount;
        render_playback_position_samples_.fetch_add(frameCount, std::memory_order_release);
        if (remaining > 0)
            return;

        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        if (reset_to_start_after_latency_drain_.exchange(false, std::memory_order_acq_rel)) {
            playback_position_samples_.store(0, std::memory_order_release);
            render_playback_position_samples_.store(0, std::memory_order_release);
        }
    }

    void SequencerEngineImpl::pumpAudio(AudioProcessContext& process) {
        const auto trackFrameCount = static_cast<int32_t>(
            std::min(static_cast<size_t>(process.frameCount()), audio_buffer_size_in_frames));

        // ── Step 1: acquire a free ring-buffer slot per track ─────────────────
        // pump_sequence_.tracks[t] is redirected to the acquired slot's context so
        // audio_preprocess_callback_ (which calls
        // timeline_->processTracksAudio(process, pump_sequence_)) writes into the
        // ring slot rather than into the shared sequence.tracks[t].
        std::fill(pump_slot_indices_.begin(), pump_slot_indices_.end(), SIZE_MAX);
        for (size_t t = 0; t < tracks_.size(); t++) {
            if (t >= pump_rings_.size() || t >= pump_sequence_.tracks.size()) {
                pump_sequence_.tracks[t] = (t < sequence.tracks.size()) ? sequence.tracks[t] : nullptr;
                continue;
            }
            size_t idx;
            if (pump_rings_[t]->free_slots.try_dequeue(idx)) {
                pump_slot_indices_[t] = idx;
                auto* ctx = pump_rings_[t]->slots[idx].ctx.get();
                ctx->eventOut().position(0);
                ctx->frameCount(trackFrameCount);
                pump_sequence_.tracks[t] = ctx;
            } else {
                // All slots full: pump is kPumpLookahead quanta ahead of RT.
                // Fall back to the shared sequence context (single-threaded path only).
                pump_sequence_.tracks[t] = (t < sequence.tracks.size()) ? sequence.tracks[t] : nullptr;
            }
        }

        // ── Step 2: fan out device input into pump contexts ───────────────────
        for (size_t t = 0; t < tracks_.size(); t++) {
            auto* ctx = pump_sequence_.tracks[t];
            if (!ctx) continue;
            for (uint32_t i = 0; i < ctx->audioInBusCount(); i++) {
                for (uint32_t ch = 0, nCh = ctx->inputChannelCount(i); ch < nCh; ch++) {
                    float* dst = ctx->getFloatInBuffer(i, ch);
                    if (process.audioInBusCount() > 0 && ch < process.inputChannelCount(0))
                        memcpy(dst, process.getFloatInBuffer(0, ch), trackFrameCount * sizeof(float));
                    else
                        memset(dst, 0, trackFrameCount * sizeof(float));
                }
            }
        }

        // ── Step 3: advance timeline and fill events / audio from clip sources ─
        // audio_preprocess_callback_ calls
        //   timeline_->processTracksAudio(process, pump_sequence_)
        // which writes clip data into pump_sequence_.tracks[t] (= ring slots).
        if (audio_preprocess_callback_)
            audio_preprocess_callback_(process);

        // ── Step 4: commit — enqueue filled slots to the RT consumer ──────────
        for (size_t t = 0; t < tracks_.size(); t++)
            if (t < pump_rings_.size() && pump_slot_indices_[t] != SIZE_MAX)
                pump_rings_[t]->filled.try_enqueue(pump_slot_indices_[t]);
    }

    int32_t SequencerEngineImpl::processAudio(AudioProcessContext& process) {
        // Record start time for deadline tracking
        auto startTime = std::chrono::steady_clock::now();

        if (tracks_.size() != sequence.tracks.size())
            // FIXME: define status codes
            return 1;

        // Clamp frame count to what track/master buffers can hold.
        const auto trackFrameCount = static_cast<int32_t>(
            std::min(static_cast<size_t>(process.frameCount()), audio_buffer_size_in_frames));

        // When engine is inactive, output silence and return.
        if (!engine_active_.load(std::memory_order_acquire)) {
            if (process.audioOutBusCount() > 0) {
                for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++)
                    memset(process.getFloatOutBuffer(0, ch), 0, process.frameCount() * sizeof(float));
            }
            return 0;
        }

        auto& data = sequence;
        bool isPlaybackActive = is_playback_active_.load(std::memory_order_acquire);
        const bool isLatencyDrainActive = latency_drain_active_.load(std::memory_order_acquire);

        // Run the pump (timeline advance + device-audio fanout + clip filling).
        // In single-threaded operation this is called here; in the Emscripten multi-threaded
        // path pumpAudio() will be driven independently from the main pthread.
        if (!external_pump_.load(std::memory_order_acquire))
            pumpAudio(process);

        // Sync MasterContext with the actual playback position *after* the pump.
        // This must live here, not in pumpAudio(), so that when the pump eventually runs
        // ahead of the audio output the UI-visible position still matches what is heard.
        {
            auto& masterContext = data.masterContext();
            masterContext.playbackPositionSamples(render_playback_position_samples_.load(std::memory_order_acquire));
            masterContext.isPlaying(isPlaybackActive || isLatencyDrainActive);
            masterContext.sampleRate(sampleRate);
        }

        // Dequeue pump slots: update sequence.tracks[t] to point to the pre-filled
        // ring-buffer slot context so the existing track-processing and mixing loops
        // use the pump-filled data without modification.  In single-threaded mode the
        // pump ran just above (pumpAudio call), so the filled queue is non-empty.
        std::fill(rt_dequeued_slots_.begin(), rt_dequeued_slots_.end(), SIZE_MAX);
        for (size_t t = 0; t < tracks_.size() && t < pump_rings_.size(); t++) {
            size_t idx;
            if (pump_rings_[t]->filled.try_dequeue(idx)) {
                rt_dequeued_slots_[t] = idx;
                sequence.tracks[t] = pump_rings_[t]->slots[idx].ctx.get();
            }
            // If no slot available: keep sequence.tracks[t] as-is (stale fallback).
        }

        // Process all tracks
        for (auto i = 0; i < sequence.tracks.size(); i++) {
            // Set processing flag BEFORE accessing sequence.tracks[i]
            track_processing_flags_[i]->store(true, std::memory_order_release);

            auto& tp = *sequence.tracks[i];
            if (!tracks_[i]->bypassed())
                tracks_[i]->graph().processAudio(tp);
            else
                tp.clearAudioOutputs();
            tp.eventIn().position(0); // reset

            // Clear processing flag AFTER we're done with the track context
            track_processing_flags_[i]->store(false, std::memory_order_release);
        }

        for (size_t i = 0; i < sequence.tracks.size() && i < tracks_.size(); ++i) {
            auto* track = tracks_[i].get();
            auto* ctx = sequence.tracks[i];
            if (!track || !ctx)
                continue;
            auto* delayLine = i < output_alignment_delay_lines_.size()
                ? &output_alignment_delay_lines_[i]
                : nullptr;
            if (!delayLine || delayLine->capacity_frames == 0)
                continue;

            bool usedDelay = false;
            const size_t startWritePosition = delayLine->write_position;
            for (uint32_t busIndex = 0; busIndex < ctx->audioOutBusCount(); ++busIndex) {
                const uint32_t outputAlignmentDelay =
                    trackOutputAlignmentHoldbackInSamplesImpl(static_cast<uapmd_track_index_t>(i), busIndex);
                if (outputAlignmentDelay == 0 || busIndex >= delayLine->buses.size())
                    continue;

                auto& busStorage = delayLine->buses[busIndex];
                const size_t maxDelayFrames = delayLine->capacity_frames > 0 ? delayLine->capacity_frames - 1 : 0;
                const size_t appliedDelayFrames = std::min<size_t>(outputAlignmentDelay, maxDelayFrames);
                size_t writePosition = startWritePosition;
                const uint32_t numChannels = std::min<uint32_t>(ctx->outputChannelCount(busIndex), static_cast<uint32_t>(busStorage.size()));
                for (uint32_t ch = 0; ch < numChannels; ++ch) {
                    auto& delayChannel = busStorage[ch];
                    float* buffer = ctx->getFloatOutBuffer(busIndex, ch);
                    if (!buffer)
                        continue;
                    writePosition = startWritePosition;
                    for (int32_t frame = 0; frame < trackFrameCount; ++frame) {
                        const float inputSample = buffer[frame];
                        delayChannel[writePosition] = inputSample;
                        const size_t readPosition =
                            (writePosition + delayLine->capacity_frames - appliedDelayFrames) %
                            delayLine->capacity_frames;
                        buffer[frame] = delayChannel[readPosition];
                        writePosition = (writePosition + 1) % delayLine->capacity_frames;
                    }
                }
                usedDelay = true;
            }
            if (usedDelay)
                delayLine->write_position = (startWritePosition + static_cast<size_t>(trackFrameCount)) % delayLine->capacity_frames;
        }

        const auto audiblePosition = playback_position_samples_.load(std::memory_order_acquire);
        const auto renderPosition = render_playback_position_samples_.load(std::memory_order_acquire);
        const bool prerollActive = isPlaybackActive && renderPosition < audiblePosition;

        // Clear main output bus (bus 0) before mixing
        if (process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++) {
                memset(process.getFloatOutBuffer(0, ch), 0, process.frameCount() * sizeof(float));
            }
        }

        auto* mixCtx = mix_bus_context_.get();
        if (mixCtx) {
            mixCtx->frameCount(trackFrameCount);
            mixCtx->clearAudioOutputs();
        }

        // Stage compensated track output buses into a dedicated mixer context so
        // downstream processing can still see per-bus structure before the final
        // master/device fold.
        for (uint32_t t = 0, nTracks = tracks_.size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventIn().position(0); // clean up *in* events here.

            if (track_output_handler_ && track_output_handler_(static_cast<int32_t>(t), *tracks_[t], *ctx))
                continue;

            if (!mixCtx)
                continue;
            for (uint32_t busIndex = 0; busIndex < ctx->audioOutBusCount(); ++busIndex)
                accumulateAudioBus(*mixCtx, busIndex, *ctx, busIndex, trackFrameCount);
        }

        // Return consumed pump slots to the free queue so the pump can reuse them.
        // Done after the mixing loop so no slot is recycled while its output buffers
        // are still being read.
        for (size_t t = 0; t < tracks_.size() && t < pump_rings_.size(); t++)
            if (rt_dequeued_slots_[t] != SIZE_MAX)
                pump_rings_[t]->free_slots.try_enqueue(rt_dequeued_slots_[t]);

        // Process master track plugins if present
        if (master_track_ && master_track_context_ && !master_track_->orderedInstanceIds().empty()) {
            auto* masterCtx = master_track_context_.get();
            masterCtx->frameCount(trackFrameCount);
            masterCtx->eventIn().position(0);
            masterCtx->eventOut().position(0);
            clearAudioInputBuses(*masterCtx);
            masterCtx->clearAudioOutputs();

            if (mixCtx && masterCtx->audioInBusCount() > 0) {
                for (uint32_t busIndex = 0; busIndex < static_cast<uint32_t>(mixCtx->audioOutBusCount()); ++busIndex) {
                    const uint32_t targetBus =
                        busIndex < static_cast<uint32_t>(masterCtx->audioInBusCount()) ? busIndex : 0;
                    accumulateAudioBusToInput(*masterCtx, targetBus, *mixCtx, busIndex, trackFrameCount);
                }
            }

            master_track_->graph().processAudio(*masterCtx);

            if (masterCtx->audioOutBusCount() > 0 && process.audioOutBusCount() > 0) {
                for (uint32_t busIndex = 0; busIndex < static_cast<uint32_t>(masterCtx->audioOutBusCount()); ++busIndex)
                    accumulateAudioBus(process, 0, *masterCtx, busIndex, trackFrameCount);
            }
        } else if (mixCtx && process.audioOutBusCount() > 0) {
            for (uint32_t busIndex = 0; busIndex < static_cast<uint32_t>(mixCtx->audioOutBusCount()); ++busIndex)
                accumulateAudioBus(process, 0, *mixCtx, busIndex, trackFrameCount);
        }

        if (prerollActive && process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++)
                memset(process.getFloatOutBuffer(0, ch), 0, process.frameCount() * sizeof(float));
        }

        // Apply soft clipping to prevent harsh distortion
        if (!prerollActive && process.audioOutBusCount() > 0) {
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

        if (isPlaybackActive) {
            render_playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);
            if (!prerollActive)
                playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);
        }
        updateLatencyDrainState(process.frameCount());

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
            applyTrackBusesLayout(master_track_.get(), AudioGraphBusesLayout{
                static_cast<uint32_t>(master_track_context_->audioInBusCount()),
                static_cast<uint32_t>(master_track_context_->audioOutBusCount()),
                1,
                1,
            });
        }
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();
    }

    uapmd_track_index_t SequencerEngineImpl::addEmptyTrack() {
        auto tr = SequencerTrack::create(
            timeline_->audioGraphProviderRegistry(),
            ump_buffer_size_in_ints,
            "");
        tracks_.emplace_back(std::move(tr));
        sequence.tracks.emplace_back(new AudioProcessContext(sequence.masterContext(), ump_buffer_size_in_ints));
        track_processing_flags_.emplace_back(std::make_unique<std::atomic<bool>>(false));
        auto trackIndex = static_cast<uapmd_track_index_t>(tracks_.size() - 1);

        // Configure main bus (moved from RealtimeSequencer)
        auto trackCtx = sequence.tracks[trackIndex];
        trackCtx->configureMainBus(default_input_channels_, default_output_channels_, audio_buffer_size_in_frames);
        applyTrackBusesLayout(tracks_[static_cast<size_t>(trackIndex)].get(), AudioGraphBusesLayout{
            static_cast<uint32_t>(trackCtx->audioInBusCount()),
            static_cast<uint32_t>(trackCtx->audioOutBusCount()),
            1,
            1,
        });

        // Create pump ring buffer for this track and configure its slot contexts.
        auto ring = std::make_unique<PumpTrackRing>(sequence.masterContext(), ump_buffer_size_in_ints);
        for (auto& slot : ring->slots)
            slot.ctx->configureMainBus(default_input_channels_, default_output_channels_, audio_buffer_size_in_frames);
        pump_rings_.emplace_back(std::move(ring));
        pump_sequence_.tracks.push_back(nullptr); // placeholder; set per-quantum in pumpAudio()

        // Keep pre-allocated work vectors in sync.
        pump_slot_indices_.resize(tracks_.size(), SIZE_MAX);
        rt_dequeued_slots_.resize(tracks_.size(), SIZE_MAX);

        // Notify timeline facade so it can create a paired TimelineTrack
        timeline_->onTrackAdded(
            default_output_channels_,
            static_cast<double>(sampleRate),
            static_cast<uint32_t>(audio_buffer_size_in_frames)
        );
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();

        return trackIndex;
    }

    bool SequencerEngineImpl::removeTrack(uapmd_track_index_t index) {
        if (index >= tracks_.size())
            return false;
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
        track_processing_flags_.erase(track_processing_flags_.begin() + static_cast<long>(index));
        if (static_cast<size_t>(index) < pump_rings_.size())
            pump_rings_.erase(pump_rings_.begin() + static_cast<long>(index));
        if (static_cast<size_t>(index) < pump_sequence_.tracks.size())
            pump_sequence_.tracks.erase(pump_sequence_.tracks.begin() + static_cast<long>(index));
        pump_slot_indices_.resize(tracks_.size(), SIZE_MAX);
        rt_dequeued_slots_.resize(tracks_.size(), SIZE_MAX);
        timeline_->onTrackRemoved(static_cast<size_t>(index));
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();
        return true;
    }

    bool SequencerEngineImpl::replaceTrackGraph(uapmd_track_index_t trackIndex, std::unique_ptr<AudioPluginGraph>&& graph) {
        SequencerTrack* track = nullptr;
        AudioProcessContext* context = nullptr;
        if (trackIndex == kMasterTrackIndex) {
            track = master_track_.get();
            context = master_track_context_.get();
        } else if (trackIndex >= 0 && static_cast<size_t>(trackIndex) < tracks_.size()) {
            track = tracks_[static_cast<size_t>(trackIndex)].get();
            if (static_cast<size_t>(trackIndex) < sequence.tracks.size())
                context = sequence.tracks[static_cast<size_t>(trackIndex)];
        }

        if (!track || !context || !graph)
            return false;
        if (!track->replaceGraph(std::move(graph)))
            return false;

        configureTrackRouting(track);
        applyTrackBusesLayout(track, AudioGraphBusesLayout{
            static_cast<uint32_t>(context->audioInBusCount()),
            static_cast<uint32_t>(context->audioOutBusCount()),
            1,
            1,
        });
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();
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

        plugin_host->createPluginInstance(static_cast<uint32_t>(sampleRate),
                                          static_cast<uint32_t>(audio_buffer_size_in_frames),
                                          default_input_channels_,
                                          default_output_channels_,
                                          false,
                                          format,
                                          pluginId,
                                          [this, trackIndex, targetMaster, callback](int32_t instanceId, std::string error) {
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

            if (targetMaster) {
                ensureContextBusConfiguration(master_track_context_.get(), instance->audioBuses());
                applyTrackBusesLayout(master_track_.get(), AudioGraphBusesLayout{
                    static_cast<uint32_t>(master_track_context_->audioInBusCount()),
                    static_cast<uint32_t>(master_track_context_->audioOutBusCount()),
                    1,
                    1,
                });
            } else {
                ensureTrackBusConfiguration(trackIndex, instance->audioBuses());
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
            plugin_host->onTrackGraphNodeAdded(
                instanceId,
                targetMaster ? kMasterTrackIndex : trackIndex,
                targetMaster,
                static_cast<uint32_t>(track->orderedInstanceIds().size() - 1));

            // Auto-assign the lowest available UMP group (0–15) on this track.
            uint8_t autoGroup = track->findAvailableGroup();
            if (autoGroup <= 15)
                track->setInstanceGroup(instanceId, autoGroup);

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
            reconfigureMixBusContext();
            reconfigureOutputAlignmentBuffers();

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

        if (const auto fbDevice = function_block_manager.getFunctionDeviceForInstance(instanceId)) {
            fbDevice->destroyDevice(instanceId);
            function_block_manager.deleteEmptyDevices();
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
                reconfigureMixBusContext();
                reconfigureOutputAlignmentBuffers();
                return true;
            }
        }
        if (master_track_ && master_track_->graph().removeNodeSimple(instanceId)) {
            refreshFunctionBlockMappings();
            reconfigureMixBusContext();
            reconfigureOutputAlignmentBuffers();
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
        timeline_->onTrackRemoved(index);
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();
    }

    // Playback control
    bool SequencerEngineImpl::isPlaybackActive() const {
        return is_playback_active_.load(std::memory_order_acquire);
    }

    void SequencerEngineImpl::playbackPosition(int64_t samples) {
        if (is_playback_active_.load(std::memory_order_acquire)) {
            resetOutputAlignmentBuffers();
            schedulePrerollFromAudiblePosition(samples);
            return;
        }
        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        reset_to_start_after_latency_drain_.store(false, std::memory_order_release);
        playback_position_samples_.store(samples, std::memory_order_release);
        render_playback_position_samples_.store(samples, std::memory_order_release);
        resetOutputAlignmentBuffers();
    }

    int64_t SequencerEngineImpl::playbackPosition() const {
        return playback_position_samples_.load(std::memory_order_acquire);
    }

    int64_t SequencerEngineImpl::renderPlaybackPosition() const {
        return render_playback_position_samples_.load(std::memory_order_acquire);
    }

    void uapmd::SequencerEngineImpl::startPlayback() {
        resetOutputAlignmentBuffers();
        schedulePrerollFromAudiblePosition(0);
        is_playback_active_.store(true, std::memory_order_release);
    }

    void uapmd::SequencerEngineImpl::stopPlayback() {
        is_playback_active_.store(false, std::memory_order_release);
        resetOutputAlignmentBuffers();
        const auto tailSamples = maxStopDrainInSamples();
        if (tailSamples > 0) {
            const auto quantum = static_cast<int64_t>(audio_buffer_size_in_frames > 0 ? audio_buffer_size_in_frames : 1);
            const auto alignedTail = ((tailSamples + quantum - 1) / quantum) * quantum;
            latency_drain_active_.store(true, std::memory_order_release);
            latency_drain_remaining_samples_.store(alignedTail, std::memory_order_release);
            reset_to_start_after_latency_drain_.store(true, std::memory_order_release);
        } else {
            playback_position_samples_.store(0, std::memory_order_release);
            render_playback_position_samples_.store(0, std::memory_order_release);
            latency_drain_active_.store(false, std::memory_order_release);
            latency_drain_remaining_samples_.store(0, std::memory_order_release);
            reset_to_start_after_latency_drain_.store(false, std::memory_order_release);
        }
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
        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        reset_to_start_after_latency_drain_.store(false, std::memory_order_release);
        render_playback_position_samples_.store(playback_position_samples_.load(std::memory_order_acquire),
                                                std::memory_order_release);
        resetOutputAlignmentBuffers();
    }

    void uapmd::SequencerEngineImpl::resumePlayback() {
        resetOutputAlignmentBuffers();
        schedulePrerollFromAudiblePosition(playback_position_samples_.load(std::memory_order_acquire));
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
}
