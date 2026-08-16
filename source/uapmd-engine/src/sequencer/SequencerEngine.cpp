#include "uapmd-midi-service/uapmd-midi-service.hpp"
#include <atomic>
#include <array>
#include <format>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <iostream>
#include <umppi/umppi.hpp>

#include <remidy/detail/event-loop.hpp>
#include <remidy/remidy.hpp>
#include "uapmd-engine/uapmd-engine.hpp"
#include "LatencyCompensationManagerImpl.hpp"
#include "TailProcessManagerImpl.hpp"
#include "TrackRoutingManager.hpp"
#include "readerwriterqueue.h"

#ifdef __EMSCRIPTEN__
#include "../devices/WebAudioWorkletIODevice.hpp"
#endif

using namespace uapmd_midi_service;
using namespace uapmd_graph;

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
        uint64_t transport_generation{0};
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

    class PreparedSequencerTrackImpl final : public PreparedSequencerTrack {
    public:
        PreparedSequencerTrackImpl(
            std::unique_ptr<SequencerTrack> track,
            AudioPluginHostingAPI& pluginHost)
            : track_(std::move(track)), plugin_host_(pluginHost) {
        }

        SequencerTrack& track() override { return *track_; }

        AudioPluginInstanceAPI* pluginInstance(int32_t instanceId) override {
            return plugin_host_.getInstance(instanceId);
        }

        AudioPluginHostingAPI& pluginHost() { return plugin_host_; }

        std::unique_ptr<SequencerTrack> releaseTrack() {
            return std::move(track_);
        }

    private:
        std::unique_ptr<SequencerTrack> track_;
        AudioPluginHostingAPI& plugin_host_;
    };

    // ─────────────────────────────────────────────────────────────────────────

    class SequencerEngineImpl : public SequencerEngine {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        uint32_t default_input_channels_{2};
        uint32_t default_output_channels_{2};
        std::vector<std::unique_ptr<SequencerTrack>> tracks_{};
        std::unique_ptr<SequencerTrack> master_track_;
        std::vector<ClipMarker> master_track_markers_{};
        std::unique_ptr<AudioProcessContext> master_track_context_;
        std::unique_ptr<AudioProcessContext> mix_bus_context_;
        SequenceProcessContext sequence{};
        int32_t sampleRate;
        std::unique_ptr<AudioPluginHostingAPI> plugin_host;
        struct PlatformMidiTarget {
            ProjectObjectId track_id;
            std::atomic<int32_t> track_index{-1};
        };
        using PlatformMidiTargets = std::vector<std::shared_ptr<PlatformMidiTarget>>;
        struct PlatformMidiRoute {
            std::string port_id;
            std::shared_ptr<MidiIOFeature> device;
            std::shared_ptr<const PlatformMidiTargets> targets;
            SequencerEngineImpl* owner{};
            moodycamel::ReaderWriterQueue<umppi::Ump> output_queue{256};
        };
        using PlatformMidiRoutes = std::vector<std::shared_ptr<PlatformMidiRoute>>;
        std::shared_ptr<const PlatformMidiRoutes> platform_midi_input_routes_;
        std::shared_ptr<const PlatformMidiRoutes> platform_midi_output_routes_;
        std::unique_ptr<MidiRecorder> midi_recorder_;
        std::vector<PlaybackEngineExtension*> playback_engine_extensions_;
        std::atomic<bool> platform_midi_output_worker_running_{true};
        std::thread platform_midi_output_worker_;
        UapmdFunctionBlockManager function_block_manager{};

        // Playback state (managed by RealtimeSequencer)
        std::atomic<bool> is_playback_active_{false};
        std::atomic<int64_t> playback_position_samples_{0};
        std::atomic<int64_t> render_playback_position_samples_{0};
        std::atomic<uint64_t> transport_generation_{0};

        // Audio preprocessing callback (for app-level source nodes)
        AudioPreprocessCallback audio_preprocess_callback_;

        // These are ordinary graph nodes, kept at the device boundaries for the
        // engine's input/output analysis APIs.
        std::unique_ptr<webaudio_compat::AnalyserNode> input_analyser_;
        webaudio_compat::AnalyserNode* output_analyser_{nullptr}; // owned by master_track_ graph

        // UMP output processing
        std::vector<uapmd_ump_t> plugin_output_scratch_;

        // Plugin instance management
        std::unordered_map<int32_t, AudioPluginInstanceAPI*> plugin_instances_;
        std::mutex instance_map_mutex_;


        // Offline rendering mode
        std::atomic<bool> offline_rendering_{false};
        std::atomic<bool> track_freeze_render_active_{false};
        bool executing_track_freeze_render_step_{false};
        struct OfflineTrackRenderSession {
            OfflineTrackRenderSettings settings;
            OfflineTrackRenderResult result;
            TimelineState previous_timeline_state;
            int64_t previous_playback_position{0};
            bool previous_offline_rendering{false};
            int64_t current_sample{0};
            remidy::MasterContext master_context;
            std::unique_ptr<AudioProcessContext> device_context;
            std::unique_ptr<AudioProcessContext> track_context;
            SequenceProcessContext render_sequence;
            std::vector<std::pair<int32_t, std::vector<uint8_t>>>
                plugin_states;
        };
        std::unique_ptr<OfflineTrackRenderSession> track_freeze_render_session_;
        using TrackAudioProcessorExtensions = std::vector<TrackAudioProcessorExtension*>;
        std::shared_ptr<const TrackAudioProcessorExtensions> track_audio_processor_extensions_{
            std::make_shared<const TrackAudioProcessorExtensions>()};
        using AudioProcessingEventHandlers = std::vector<AudioProcessingEventHandler*>;
        std::shared_ptr<const AudioProcessingEventHandlers> audio_processing_event_handlers_{
            std::make_shared<const AudioProcessingEventHandlers>()};
        std::vector<SequencerProcessingLifecycleListener*> processing_lifecycle_listeners_;
        std::vector<PluginInstanceLifecycleListener*> plugin_instance_lifecycle_listeners_;
        // Engine active flag: when false, processAudio outputs silence without invoking plugins.
        // Starts inactive so that no plugin code runs before the user explicitly enables the
        // audio engine (important on Emscripten where AudioWorklet fires immediately after
        // connect before lazy-initialized statics are guaranteed to be ready).
        std::atomic<bool> engine_active_{false};

        // Output mute flag: when true, the graph still processes every cycle (plugin
        // tails render out, spectra update) but the device output bus is silenced.
        std::atomic<bool> output_muted_{false};

        // Structural-mutation handshake (Dekker pattern). Main-thread mutations of the
        // parallel per-track vectors (tracks_ / sequence.tracks / track_processing_flags_ /
        // pump_*) must never overlap a processAudio() walk: vector erase/emplace invalidates
        // the storage processAudio() is indexing, which crashes on e.g. project reload where
        // loadProject() removes every track while audio keeps running. Mutators raise
        // structure_mutation_active_ and spin until the audio thread is observed outside
        // processAudio(); processAudio() announces itself via in_process_audio_ FIRST, then
        // re-checks the mutation flag and backs out with silence if one is (or went) in
        // flight. Both sides use seq_cst on the store->load pair so the store-load ordering
        // that the handshake depends on cannot be broken.
        std::atomic<bool> structure_mutation_active_{false};
        std::atomic<bool> in_process_audio_{false};

        // RAII for the mutator side. Held only on the main thread, never nested by the
        // current call graph (no track mutator calls another). The spin is bounded by one
        // audio callback duration (a few ms at most); cleanupEmptyTracks() already relies on
        // the same busy-wait idiom.
        struct StructureMutationGuard {
            SequencerEngineImpl& engine;
            explicit StructureMutationGuard(SequencerEngineImpl& e) : engine(e) {
                engine.structure_mutation_active_.store(true, std::memory_order_seq_cst);
                while (engine.in_process_audio_.load(std::memory_order_seq_cst))
                    std::this_thread::yield();
            }
            ~StructureMutationGuard() {
                engine.structure_mutation_active_.store(false, std::memory_order_release);
            }
        };

        // RAII for the audio-thread side, so every return path in processAudio() clears it.
        struct InProcessAudioScope {
            std::atomic<bool>& flag;
            explicit InProcessAudioScope(std::atomic<bool>& f) : flag(f) {
                flag.store(true, std::memory_order_seq_cst);
            }
            ~InProcessAudioScope() {
                flag.store(false, std::memory_order_release);
            }
        };

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
        std::unique_ptr<TrackRoutingManager> track_routing_manager_{};
        std::unique_ptr<LatencyCompensationManagerImpl> latency_compensation_manager_{};
        std::unique_ptr<TailProcessManagerImpl> tail_process_manager_{};

        void ensureTrackBusConfiguration(int32_t trackIndex, remidy::PluginAudioBuses* pluginBuses);
        void ensureContextBusConfiguration(AudioProcessContext* ctx, remidy::PluginAudioBuses* pluginBuses);
        std::vector<remidy::AudioBusSpec> mergeBusSpecs(const std::vector<remidy::AudioBusSpec>& current,
                                                        const std::vector<remidy::AudioBusConfiguration*>& pluginBuses);

        // Timeline facade (owns timeline tracks, clips, project loading)
        std::unique_ptr<TimelineFacade> timeline_;
        std::unique_ptr<FrozenTrackManager> frozen_track_manager_;

    public:
        void registerAddinExtensionPoints(uapmd_addin::AddinManager& manager) override {
            manager.registerExtensionPoint("/uapmd/engine/v1", this);
        }

        explicit SequencerEngineImpl(
            int32_t sampleRate,
            size_t audioBufferSizeInFrames,
            size_t umpBufferSizeInInts,
            std::unique_ptr<AudioPluginHostingAPI> suppliedPluginHost = {});
        ~SequencerEngineImpl() override;

        AudioPluginHostingAPI* pluginHost() override;
        FrozenTrackManager& frozenTrackManager() override { return *frozen_track_manager_; }
        TailProcessManager& tailProcessManager() override { return *tail_process_manager_; }

        SequenceProcessContext& data() override { return sequence; }

        std::vector<SequencerTrack*>& tracks() const override;
        SequencerTrack* masterTrack() override;
        const std::vector<ClipMarker>& masterTrackMarkers() const override;
        void setMasterTrackMarkers(std::vector<ClipMarker> markers) override;
        size_t umpBufferSizeInBytes() const override { return ump_buffer_size_in_ints; }
        uint32_t trackLatencyInSamples(uapmd_track_index_t trackIndex) override;
        uint32_t masterTrackLatencyInSamples() override;
        uint32_t trackRenderLeadInSamples(uapmd_track_index_t trackIndex) override;
        uint32_t masterTrackRenderLeadInSamples() override;
        bool trackHasLiveInput(uapmd_track_index_t trackIndex) override;
        LatencyCompensationManager* latencyCompensationManager() override;
        uint32_t trackOutputAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex) override;
        uint32_t trackOutputBusAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) override;
        TrackOutputRoutingTarget trackOutputBusRoutingTarget(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) override;
        std::vector<TrackOutputRoutingRule> trackOutputRoutingRules(uapmd_track_index_t trackIndex) override;
        void setTrackOutputRoutingRules(
            uapmd_track_index_t trackIndex,
            const std::vector<TrackOutputRoutingRule>& rules) override;
        bool isOutputAlignmentActive() override;

        void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;
        void setSampleRate(int32_t newSampleRate) override;
        uapmd_track_index_t addEmptyTrack(
            uapmd_track_index_t insertionIndex = -1) override;
        std::unique_ptr<PreparedSequencerTrack> prepareTrack(
            const std::string& graphProviderId = {}) override;
        void addPluginToPreparedTrack(
            PreparedSequencerTrack& prepared,
            std::string& format,
            std::string& pluginId,
            std::function<void(int32_t instanceId, std::string error)> callback,
            std::string restoreNodeId = {}) override;
        uapmd_track_index_t publishPreparedTrack(
            std::unique_ptr<PreparedSequencerTrack> prepared,
            uapmd_track_index_t insertionIndex = -1) override;
        bool removeTrack(uapmd_track_index_t trackIndex) override;
        bool replaceTrackGraph(uapmd_track_index_t trackIndex, std::unique_ptr<AudioPluginGraph>&& graph) override;
        void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback, std::string restoreNodeId = {}) override;
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
            if (!executing_track_freeze_render_step_ &&
                frozen_track_manager_->isInstanceBusy(instanceId))
                return false;
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

        void addTrackAudioProcessorExtension(TrackAudioProcessorExtension& extension) override {
            auto current = std::atomic_load_explicit(
                &track_audio_processor_extensions_, std::memory_order_acquire);
            if (current && std::find(current->begin(), current->end(), &extension) != current->end())
                return;
            auto updated = std::make_shared<TrackAudioProcessorExtensions>(
                current ? *current : TrackAudioProcessorExtensions{});
            updated->push_back(&extension);
            std::shared_ptr<const TrackAudioProcessorExtensions> published = std::move(updated);
            std::atomic_store_explicit(
                &track_audio_processor_extensions_, std::move(published), std::memory_order_release);
        }
        void removeTrackAudioProcessorExtension(TrackAudioProcessorExtension& extension) override {
            auto current = std::atomic_load_explicit(
                &track_audio_processor_extensions_, std::memory_order_acquire);
            if (!current)
                return;
            auto updated = std::make_shared<TrackAudioProcessorExtensions>(*current);
            std::erase(*updated, &extension);
            std::shared_ptr<const TrackAudioProcessorExtensions> published = std::move(updated);
            std::atomic_store_explicit(
                &track_audio_processor_extensions_, std::move(published), std::memory_order_release);
        }
        void addAudioProcessingEventHandler(AudioProcessingEventHandler& handler) override {
            auto current = std::atomic_load_explicit(
                &audio_processing_event_handlers_, std::memory_order_acquire);
            if (current && std::find(current->begin(), current->end(), &handler) != current->end())
                return;
            auto updated = std::make_shared<AudioProcessingEventHandlers>(
                current ? *current : AudioProcessingEventHandlers{});
            updated->push_back(&handler);
            std::shared_ptr<const AudioProcessingEventHandlers> published = std::move(updated);
            std::atomic_store_explicit(
                &audio_processing_event_handlers_, std::move(published), std::memory_order_release);
        }
        void removeAudioProcessingEventHandler(AudioProcessingEventHandler& handler) override {
            auto current = std::atomic_load_explicit(
                &audio_processing_event_handlers_, std::memory_order_acquire);
            if (!current)
                return;
            auto updated = std::make_shared<AudioProcessingEventHandlers>(*current);
            std::erase(*updated, &handler);
            std::shared_ptr<const AudioProcessingEventHandlers> published = std::move(updated);
            std::atomic_store_explicit(
                &audio_processing_event_handlers_, std::move(published), std::memory_order_release);
        }
        void addProcessingLifecycleListener(
            SequencerProcessingLifecycleListener& listener) override {
            if (std::find(
                    processing_lifecycle_listeners_.begin(),
                    processing_lifecycle_listeners_.end(),
                    &listener) == processing_lifecycle_listeners_.end())
                processing_lifecycle_listeners_.push_back(&listener);
        }
        void removeProcessingLifecycleListener(
            SequencerProcessingLifecycleListener& listener) override {
            std::erase(processing_lifecycle_listeners_, &listener);
        }
        void addPluginInstanceLifecycleListener(
            PluginInstanceLifecycleListener& listener) override {
            if (std::find(
                    plugin_instance_lifecycle_listeners_.begin(),
                    plugin_instance_lifecycle_listeners_.end(),
                    &listener) == plugin_instance_lifecycle_listeners_.end())
                plugin_instance_lifecycle_listeners_.push_back(&listener);
        }
        void removePluginInstanceLifecycleListener(
            PluginInstanceLifecycleListener& listener) override {
            std::erase(plugin_instance_lifecycle_listeners_, &listener);
        }
        void addPlaybackEngineExtension(PlaybackEngineExtension& extension) override {
            if (std::find(playback_engine_extensions_.begin(), playback_engine_extensions_.end(), &extension) ==
                playback_engine_extensions_.end())
                playback_engine_extensions_.push_back(&extension);
        }
        void removePlaybackEngineExtension(PlaybackEngineExtension& extension) override {
            std::erase(playback_engine_extensions_, &extension);
        }
        PlaybackEngineExtension* findPlaybackEngineExtension(std::string_view extensionId) override {
            for (auto* extension : playback_engine_extensions_)
                if (extension && extension->extensionId() == extensionId)
                    return extension;
            return nullptr;
        }
        void notifyRecordingStarted() override {
            for (auto* extension : playback_engine_extensions_)
                if (extension)
                    extension->recordingStarted();
        }
        void notifyRecordingStopped() override {
            for (auto* extension : playback_engine_extensions_)
                if (extension)
                    extension->recordingStopped();
        }
        void notifyTrackAudioContentChanged(uapmd_track_index_t trackIndex) {
            auto extensions = std::atomic_load_explicit(
                &track_audio_processor_extensions_, std::memory_order_acquire);
            if (!extensions)
                return;
            for (auto* extension : *extensions)
                if (extension)
                    extension->audioContentChanged(*this, trackIndex);
        }

        void pumpAudio(AudioProcessContext& process) override;
        uapmd_status_t processAudio(AudioProcessContext& process) override;
        bool beginOfflineTrackRender(
            const OfflineTrackRenderSettings& settings,
            std::string& error) override;
        OfflineTrackRenderStepResult renderOfflineTrackStep(
            uint32_t maximumBlocks) override;
        OfflineTrackRenderResult finishOfflineTrackRender(
            bool canceled,
            const std::function<void(OfflineTrackRenderResult&)>& transition) override;
        OfflineTrackRenderResult renderOfflineTrack(
            const OfflineTrackRenderSettings& settings,
            const OfflineRenderCallbacks& callbacks) override;

        // Playback control
        bool isPlaybackActive() const override;
        void playbackPosition(int64_t samples) override;
        int64_t playbackPosition() const override;
        int32_t currentSampleRate() const override { return sampleRate; }
        int64_t renderPlaybackPosition() const override;
        void jumpPlayback(double positionSeconds) override;
        void startPlayback() override;
        void stopPlayback() override;
        void pausePlayback() override;
        void resumePlayback() override;

        // Audio analysis
        webaudio_compat::AnalyserNode* inputAnalyser() override;
        webaudio_compat::AnalyserNode* outputAnalyser() override;

        // Plugin instance queries
        AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) override;

        UapmdFunctionBlockManager *functionBlockManager() override { return &function_block_manager; }
        int32_t findTrackIndexForInstance(int32_t instanceId) const override;

        // Event routing
        void enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) override;
        bool connectPlatformMidiInputToTrack(
            std::string portId, ProjectObjectId trackId) override;
        void disconnectPlatformMidiInputFromTrack(std::string_view portId, std::string_view trackId) override;
        std::vector<MidiPortTrackConnection> platformMidiInputConnections() const override;
        void clearPlatformMidiInputRoute() override;
        bool connectPlatformMidiOutputToTrack(
            std::string portId, ProjectObjectId trackId) override;
        void disconnectPlatformMidiOutputFromTrack(std::string_view portId, std::string_view trackId) override;
        std::vector<MidiPortTrackConnection> platformMidiOutputConnections() const override;
        void clearPlatformMidiOutputRoute() override;

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

        void setOutputMuted(bool muted) override {
            output_muted_.store(muted, std::memory_order_release);
        }

        void resetProcessingState() override;
        void resetTrackProcessingState(
            uapmd_track_index_t trackIndex,
            bool resetPlugins,
            const std::function<void()>& transition) override;

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
        static void platformMidiInputTrampoline(
            void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void deliverPlatformMidiInput(
            PlatformMidiRoute& route, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void enqueuePlatformMidiOutput(int32_t trackIndex, const uapmd_ump_t* ump, size_t sizeInBytes);
        void runPlatformMidiOutputWorker();
        void removePlatformMidiTrackConnections(std::string_view trackId);
        void refreshPlatformMidiTrackIndices();
        void requestAllNotesOff();
        void applyLatencyCompensationTimingUpdateLocked();
        TrackOutputRoutingTarget effectiveTrackOutputBusRoutingTarget(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        uint32_t trackOutputAlignmentHoldbackInSamplesImpl(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) const;
        void reconfigureMasterTrackInputBuses();
        void reconfigureMixBusContext();
        void reconfigureOutputAlignmentBuffers();
        void resetOutputAlignmentBuffers();
        void notifyAudioProcessingConfigurationChanged();
        void notifyPluginGraphChanged();
        void notifyPluginInstanceAdded(int32_t instanceId, AudioPluginInstanceAPI& instance);
        void notifyGraphTimingChanged();
        void notifyPluginInstanceWillBeDestroyed(int32_t instanceId);
        void notifyTrackProcessingStateReset(uapmd_track_index_t trackIndex);
        void notifyProcessingStateReset();
        void notifyTransportTransition(
            SequencerTransportTransition transition,
            int64_t audiblePositionSamples);
        void clearTrackProcessingState(
            uapmd_track_index_t trackIndex,
            bool resetPlugins);
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

    std::unique_ptr<SequencerEngine> SequencerEngine::createWithPluginHost(
        int32_t sampleRate,
        size_t audioBufferSizeInFrames,
        size_t umpBufferSizeInInts,
        std::unique_ptr<AudioPluginHostingAPI> pluginHost) {
        return std::make_unique<SequencerEngineImpl>(
            sampleRate,
            audioBufferSizeInFrames,
            umpBufferSizeInInts,
            std::move(pluginHost));
    }

    // SequencerEngineImpl
    SequencerEngineImpl::SequencerEngineImpl(
        int32_t sampleRate,
        size_t audioBufferSizeInFrames,
        size_t umpBufferSizeInInts,
        std::unique_ptr<AudioPluginHostingAPI> suppliedPluginHost) :
        audio_buffer_size_in_frames(audioBufferSizeInFrames),
        sampleRate(sampleRate),
        ump_buffer_size_in_ints(umpBufferSizeInInts),
        plugin_host(suppliedPluginHost
            ? std::move(suppliedPluginHost)
            : AudioPluginHostingAPI::create()),
        plugin_output_scratch_(umpBufferSizeInInts, 0) {
        input_analyser_ = webaudio_compat::createAnalyserNode({.node_id = "engine-input-analyser"});
        timeline_ = TimelineFacade::create(*this);
        midi_recorder_ = std::make_unique<MidiRecorder>(*this);
        addPlaybackEngineExtension(*midi_recorder_);
        tail_process_manager_ = std::make_unique<TailProcessManagerImpl>(
            audio_buffer_size_in_frames,
            this->sampleRate,
            is_playback_active_,
            playback_position_samples_,
            render_playback_position_samples_);
        frozen_track_manager_ = std::make_unique<FrozenTrackManager>(*this, *timeline_);
        timeline_->addProjectSerializationExtension(frozen_track_manager_->projectSerializationExtension());
        addTrackAudioProcessorExtension(frozen_track_manager_->audioProcessorExtension());
        master_track_ = SequencerTrack::create(
            timeline_->audioGraphProviderRegistry(),
            umpBufferSizeInInts,
            "");
        if (master_track_) {
            AudioGraphNodeDescriptor outputAnalyserDescriptor;
            outputAnalyserDescriptor.node_id = "engine-output-analyser";
            outputAnalyserDescriptor.node_type = std::string(webaudio_compat::kAnalyserNodeType);
            outputAnalyserDescriptor.display_name = "Output Analyser";
            if (master_track_->graph().appendBuiltInNodeSimple(outputAnalyserDescriptor) == 0)
                output_analyser_ = dynamic_cast<webaudio_compat::AnalyserNode*>(
                    master_track_->graph().getNode(outputAnalyserDescriptor.node_id));
        }
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
        latency_compensation_manager_ = std::make_unique<LatencyCompensationManagerImpl>(
            audio_buffer_size_in_frames,
            tracks_,
            master_track_,
            sequence,
            is_playback_active_,
            playback_position_samples_,
            render_playback_position_samples_,
            [this](const std::function<void()>& mutation) {
                StructureMutationGuard mutationGuard(*this);
                mutation();
            },
            [this](int32_t instanceId) {
                return getPluginInstance(instanceId);
            },
            [this]() {
                tail_process_manager_->cancelTailProcessing();
                requestAllNotesOff();
                transport_generation_.fetch_add(1, std::memory_order_release);
            });
        timeline_->addProjectSerializationExtension(*latency_compensation_manager_);
        track_routing_manager_ = std::make_unique<TrackRoutingManager>(
            audio_buffer_size_in_frames,
            sampleRate,
            default_output_channels_,
            tracks_,
            master_track_,
            master_track_context_,
            mix_bus_context_,
            sequence,
            timeline_.get(),
            *latency_compensation_manager_);
        latency_compensation_manager_->attachTrackRoutingManager(*track_routing_manager_);
        addAudioProcessingEventHandler(*latency_compensation_manager_);
        addProcessingLifecycleListener(*latency_compensation_manager_);
        reconfigureMixBusContext();
        configureTrackRouting(master_track_.get());
        platform_midi_output_worker_ = std::thread([this] { runPlatformMidiOutputWorker(); });

        // Call the pump-aware overload so that processTracksAudio writes into
        // pump_sequence_.tracks[i] (ring-buffer slots) instead of sequence.tracks[i].
        audio_preprocess_callback_ = [this](AudioProcessContext& process) {
            timeline_->processTracksAudio(process, pump_sequence_);
        };
        notifyAudioProcessingConfigurationChanged();
    }

    SequencerEngineImpl::~SequencerEngineImpl() {
        clearPlatformMidiInputRoute();
        clearPlatformMidiOutputRoute();
        platform_midi_output_worker_running_.store(false, std::memory_order_release);
        if (platform_midi_output_worker_.joinable())
            platform_midi_output_worker_.join();
        if (frozen_track_manager_) {
            removeTrackAudioProcessorExtension(frozen_track_manager_->audioProcessorExtension());
            timeline_->removeProjectSerializationExtension(frozen_track_manager_->projectSerializationExtension());
            frozen_track_manager_.reset();
        }
        if (latency_compensation_manager_)
            latency_compensation_manager_->clearPluginTimingListeners();
        if (latency_compensation_manager_) {
            removeAudioProcessingEventHandler(*latency_compensation_manager_);
            removeProcessingLifecycleListener(*latency_compensation_manager_);
            timeline_->removeProjectSerializationExtension(*latency_compensation_manager_);
        }
        tail_process_manager_.reset();
        // Detach output mappers while plugin instances are still alive. This is a separate
        // step from clearAllDevices() because AppModel::DeviceState holds shared_ptrs to
        // UapmdFunctionBlock that may outlive the engine — detaching now ensures those
        // delayed destructions won't access freed PluginParameterSupport objects.
        function_block_manager.detachAllOutputMappers();
        function_block_manager.clearAllDevices();
        // Detach the timeline's plug-in parameter observers before graph
        // destruction invalidates the hosted instances they reference.
        timeline_.reset();
        // Make sure to clean up all track graphs before plugin_host so that
        // AudioPluginNodeImpl destructors can still touch the live instances.
        tracks_.clear();
        master_track_.reset();
    }

    void SequencerEngineImpl::applyLatencyCompensationTimingUpdateLocked() {
        tail_process_manager_->cancelTailProcessing();
        requestAllNotesOff();
        transport_generation_.fetch_add(1, std::memory_order_release);
        if (track_routing_manager_)
            track_routing_manager_->rebuildRoutingCaches();
        notifyGraphTimingChanged();
    }

    void SequencerEngineImpl::notifyAudioProcessingConfigurationChanged() {
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->audioProcessingConfigurationChanged();
    }

    void SequencerEngineImpl::notifyPluginGraphChanged() {
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->pluginGraphChanged();
    }

    void SequencerEngineImpl::notifyPluginInstanceAdded(
        int32_t instanceId,
        AudioPluginInstanceAPI& instance) {
        for (auto* listener : plugin_instance_lifecycle_listeners_)
            if (listener)
                listener->pluginInstanceAdded(instanceId, instance);
    }

    void SequencerEngineImpl::notifyGraphTimingChanged() {
        const bool isPlaybackActive =
            is_playback_active_.load(std::memory_order_acquire);
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->graphTimingChanged(isPlaybackActive);
    }

    void SequencerEngineImpl::notifyPluginInstanceWillBeDestroyed(
        int32_t instanceId) {
        for (auto* listener : plugin_instance_lifecycle_listeners_)
            if (listener)
                listener->pluginInstanceWillBeDestroyed(instanceId);
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->pluginInstanceWillBeDestroyed(instanceId);
    }

    void SequencerEngineImpl::notifyTrackProcessingStateReset(
        uapmd_track_index_t trackIndex) {
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->trackProcessingStateReset(trackIndex);
    }

    void SequencerEngineImpl::notifyProcessingStateReset() {
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->processingStateReset();
    }

    void SequencerEngineImpl::notifyTransportTransition(
        SequencerTransportTransition transition,
        int64_t audiblePositionSamples) {
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->transportTransition(transition, audiblePositionSamples);
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

    const std::vector<ClipMarker>& SequencerEngineImpl::masterTrackMarkers() const {
        return master_track_markers_;
    }

    void SequencerEngineImpl::setMasterTrackMarkers(std::vector<ClipMarker> markers) {
        master_track_markers_ = std::move(markers);
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
        return track_routing_manager_
            ? track_routing_manager_->trackAudibleRenderLeadInSamples(trackIndex)
            : 0;
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
        return effectiveTrackOutputBusRoutingTarget(trackIndex, outputBusIndex);
    }

    std::vector<TrackOutputRoutingRule> SequencerEngineImpl::trackOutputRoutingRules(uapmd_track_index_t trackIndex) {
        return track_routing_manager_
            ? track_routing_manager_->trackOutputRoutingRules(trackIndex)
            : std::vector<TrackOutputRoutingRule>{};
    }

    void SequencerEngineImpl::setTrackOutputRoutingRules(
        uapmd_track_index_t trackIndex,
        const std::vector<TrackOutputRoutingRule>& rules) {
        if (frozen_track_manager_->isTrackBusy(trackIndex))
            return;
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
            return;
        StructureMutationGuard mutationGuard(*this);
        if (track_routing_manager_)
            track_routing_manager_->setTrackOutputRoutingRules(trackIndex, rules);
        reconfigureMixBusContext();
        reconfigureMasterTrackInputBuses();
        reconfigureOutputAlignmentBuffers();
        applyLatencyCompensationTimingUpdateLocked();
    }

    bool SequencerEngineImpl::isOutputAlignmentActive() {
        return track_routing_manager_ && track_routing_manager_->isOutputAlignmentActive();
    }

    LatencyCompensationManager* SequencerEngineImpl::latencyCompensationManager() {
        return latency_compensation_manager_.get();
    }

    TrackOutputRoutingTarget SequencerEngineImpl::effectiveTrackOutputBusRoutingTarget(
        uapmd_track_index_t trackIndex,
        uint32_t outputBusIndex) const {
        return track_routing_manager_
            ? track_routing_manager_->effectiveTrackOutputBusRoutingTarget(trackIndex, outputBusIndex)
            : TrackOutputRoutingTarget{};
    }

    uint32_t SequencerEngineImpl::trackOutputAlignmentHoldbackInSamplesImpl(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) const {
        return track_routing_manager_
            ? track_routing_manager_->trackOutputAlignmentHoldbackInSamples(trackIndex, outputBusIndex)
            : 0;
    }

    void SequencerEngineImpl::reconfigureMasterTrackInputBuses() {
        if (track_routing_manager_)
            track_routing_manager_->reconfigureMasterTrackInputBuses();
    }

    void SequencerEngineImpl::reconfigureMixBusContext() {
        if (track_routing_manager_)
            track_routing_manager_->reconfigureMixBusContext();
    }

    void SequencerEngineImpl::reconfigureOutputAlignmentBuffers() {
        notifyAudioProcessingConfigurationChanged();
    }

    void SequencerEngineImpl::resetOutputAlignmentBuffers() {
        notifyProcessingStateReset();
    }

    void SequencerEngineImpl::resetProcessingState() {
        auto clearContextBuffers = [](AudioProcessContext* ctx) {
            if (!ctx)
                return;
            ctx->clearAudioInputs();
            ctx->clearAudioOutputs();
            ctx->eventIn().position(0);
            ctx->eventOut().position(0);
        };

        // Drain pump rings: return filled slots to the free queue and clear every slot.
        for (auto& ring : pump_rings_) {
            if (!ring)
                continue;
            size_t idx;
            while (ring->filled.try_dequeue(idx))
                ring->free_slots.try_enqueue(idx);
            for (auto& slot : ring->slots)
                clearContextBuffers(slot.ctx.get());
        }

        // sequence.tracks entries point at ring slots (already cleared above) or at the
        // default track contexts; pump_sequence_ mirrors them for the pump side.
        for (auto* ctx : sequence.tracks)
            clearContextBuffers(ctx);
        for (auto* ctx : pump_sequence_.tracks)
            clearContextBuffers(ctx);
        clearContextBuffers(mix_bus_context_.get());
        clearContextBuffers(master_track_context_.get());

        resetOutputAlignmentBuffers();

        // Reset any leftover tail processing so a restart does not continue a drain.
        tail_process_manager_->cancelTailProcessing();

        // Drop events that were queued for plugins but never delivered; replaying them
        // on restart would trigger stale notes and parameter changes.
        auto clearTrackEvents = [](SequencerTrack* track) {
            if (!track)
                return;
            for (auto& entry : track->graph().plugins())
                if (entry.second)
                    entry.second->clearQueuedEvents();
        };
        for (auto& track : tracks_)
            clearTrackEvents(track.get());
        clearTrackEvents(master_track_.get());

        if (input_analyser_)
            input_analyser_->reset();
        if (output_analyser_)
            output_analyser_->reset();
    }

    void SequencerEngineImpl::clearTrackProcessingState(
        uapmd_track_index_t trackIndex,
        bool resetPlugins) {
        if (trackIndex < 0 ||
            static_cast<size_t>(trackIndex) >= tracks_.size())
            return;

        const auto index = static_cast<size_t>(trackIndex);
        auto clearContext = [](AudioProcessContext* context) {
            if (!context)
                return;
            context->clearAudioInputs();
            context->clearAudioOutputs();
            context->eventIn().position(0);
            context->eventOut().position(0);
        };
        if (index < sequence.tracks.size())
            clearContext(sequence.tracks[index]);
        if (index < pump_sequence_.tracks.size())
            clearContext(pump_sequence_.tracks[index]);
        if (index < pump_rings_.size() && pump_rings_[index])
            for (auto& slot : pump_rings_[index]->slots)
                clearContext(slot.ctx.get());
        notifyTrackProcessingStateReset(trackIndex);

        if (!resetPlugins || !tracks_[index])
            return;
        for (const auto instanceId : tracks_[index]->orderedInstanceIds()) {
            auto* instance = getPluginInstance(instanceId);
            if (!instance)
                continue;
            instance->stopProcessing();
            instance->startProcessing();
        }
    }

    void SequencerEngineImpl::resetTrackProcessingState(
        uapmd_track_index_t trackIndex,
        bool resetPlugins,
        const std::function<void()>& transition) {
        StructureMutationGuard mutationGuard(*this);
        clearTrackProcessingState(trackIndex, resetPlugins);
        if (transition)
            transition();
    }

    void SequencerEngineImpl::pumpAudio(AudioProcessContext& process) {
        const auto transportGeneration =
            transport_generation_.load(std::memory_order_acquire);
        const auto trackFrameCount = static_cast<int32_t>(
            std::min(static_cast<size_t>(process.frameCount()), audio_buffer_size_in_frames));

        // Track add/remove on the main thread updates tracks_ and the pump-side
        // vectors non-atomically, so clamp every loop below to the smallest size
        // and skip the not-yet-published tracks for this quantum (lock-free).
        const size_t pumpTrackCount = std::min(
            std::min(tracks_.size(), pump_sequence_.tracks.size()),
            std::min(pump_rings_.size(), pump_slot_indices_.size()));

        // ── Step 1: acquire a free ring-buffer slot per track ─────────────────
        // pump_sequence_.tracks[t] is redirected to the acquired slot's context so
        // audio_preprocess_callback_ (which calls
        // timeline_->processTracksAudio(process, pump_sequence_)) writes into the
        // ring slot rather than into the shared sequence.tracks[t].
        std::fill(pump_slot_indices_.begin(), pump_slot_indices_.end(), SIZE_MAX);
        for (size_t t = 0; t < pumpTrackCount; t++) {
            size_t idx;
            if (pump_rings_[t]->free_slots.try_dequeue(idx)) {
                pump_slot_indices_[t] = idx;
                auto& slot = pump_rings_[t]->slots[idx];
                slot.transport_generation = transportGeneration;
                auto* ctx = slot.ctx.get();
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
        for (size_t t = 0; t < pumpTrackCount; t++) {
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
        for (size_t t = 0; t < pumpTrackCount; t++)
            if (pump_slot_indices_[t] != SIZE_MAX)
                pump_rings_[t]->filled.try_enqueue(pump_slot_indices_[t]);
    }

    int32_t SequencerEngineImpl::processAudio(AudioProcessContext& process) {
        // Record start time for deadline tracking
        auto startTime = std::chrono::steady_clock::now();

        // Structural-mutation handshake: announce we're inside the audio walk before
        // anything touches the per-track vectors, then back out with silence if a
        // main-thread mutation is in flight (see structure_mutation_active_).
        InProcessAudioScope inProcessAudio(in_process_audio_);
        if (structure_mutation_active_.load(std::memory_order_seq_cst) ||
            track_freeze_render_active_.load(std::memory_order_seq_cst)) {
            process.clearAudioOutputs();
            return 0;
        }

        if (tracks_.size() != sequence.tracks.size()) {
            process.clearAudioOutputs();
            // FIXME: define status codes
            return 1;
        }

        // Clamp frame count to what track/master buffers can hold.
        const auto trackFrameCount = static_cast<int32_t>(
            std::min(static_cast<size_t>(process.frameCount()), audio_buffer_size_in_frames));

        // When engine is inactive, output silence and return.
        if (!engine_active_.load(std::memory_order_acquire)) {
            process.clearAudioOutputs();
            return 0;
        }

        // The input analyser is an ordinary pass-through node at the device
        // boundary. Its copied output is not part of the main mix; the normal
        // track routing below owns that output, so discard this tap's buffer.
        if (input_analyser_) {
            input_analyser_->processAudio(process);
            process.clearAudioOutputs();
        }

        auto& data = sequence;
        bool isPlaybackActive = is_playback_active_.load(std::memory_order_acquire);
        const bool isTailDrainActive =
            tail_process_manager_->tailDrainActive();

        // Run the pump (timeline advance + device-audio fanout + clip filling).
        pumpAudio(process);

        // Sync MasterContext with the actual playback position *after* the pump.
        // This must live here, not in pumpAudio(), so that when the pump eventually runs
        // ahead of the audio output the UI-visible position still matches what is heard.
        {
            auto& masterContext = data.masterContext();
            masterContext.playbackPositionSamples(render_playback_position_samples_.load(std::memory_order_acquire));
            masterContext.isPlaying(isPlaybackActive || isTailDrainActive);
            masterContext.sampleRate(sampleRate);
        }

        // Dequeue pump slots: update sequence.tracks[t] to point to the pre-filled
        // ring-buffer slot context so the existing track-processing and mixing loops
        // use the pump-filled data without modification.  In single-threaded mode the
        // pump ran just above (pumpAudio call), so the filled queue is non-empty.
        std::fill(rt_dequeued_slots_.begin(), rt_dequeued_slots_.end(), SIZE_MAX);
        // Same clamping rationale as pumpAudio(): the pump-side vectors may lag
        // tracks_/sequence.tracks while the main thread is adding a track.
        const size_t rtPumpTrackCount = std::min(
            std::min(tracks_.size(), sequence.tracks.size()),
            std::min(pump_rings_.size(), rt_dequeued_slots_.size()));
        for (size_t t = 0; t < rtPumpTrackCount; t++) {
            size_t idx;
            if (pump_rings_[t]->filled.try_dequeue(idx)) {
                rt_dequeued_slots_[t] = idx;
                auto& slot = pump_rings_[t]->slots[idx];
                sequence.tracks[t] = slot.ctx.get();
                if (slot.transport_generation !=
                    transport_generation_.load(std::memory_order_acquire)) {
                    sequence.tracks[t]->clearAudioInputs();
                    sequence.tracks[t]->clearAudioOutputs();
                    sequence.tracks[t]->eventIn().position(0);
                    sequence.tracks[t]->eventOut().position(0);
                }
            }
            // If no slot available: keep sequence.tracks[t] as-is (stale fallback).
        }

        // Process all tracks (track_processing_flags_ may lag sequence.tracks
        // while the main thread is adding a track, hence the extra clamp).
        const size_t processTrackCount = std::min(
            std::min(tracks_.size(), sequence.tracks.size()),
            track_processing_flags_.size());
        for (size_t i = 0; i < processTrackCount; i++) {
            // Set processing flag BEFORE accessing sequence.tracks[i]
            track_processing_flags_[i]->store(true, std::memory_order_release);

            auto& tp = *sequence.tracks[i];
            const TrackAudioProcessingEvent event{
                static_cast<uapmd_track_index_t>(i),
                *tracks_[i],
                tp,
                trackFrameCount,
            };
            auto eventHandlers = std::atomic_load_explicit(
                &audio_processing_event_handlers_, std::memory_order_acquire);
            if (eventHandlers)
                for (auto* handler : *eventHandlers)
                    if (handler)
                        handler->beforeTrackProcess(event);
            bool processedByExtension = false;
            auto extensions = std::atomic_load_explicit(
                &track_audio_processor_extensions_, std::memory_order_acquire);
            if (extensions) {
                for (auto* extension : *extensions) {
                    if (!extension || !extension->shouldProcessAudio(
                            *this,
                            static_cast<uapmd_track_index_t>(i),
                            *tracks_[i],
                            tp))
                        continue;
                    extension->processAudio(
                        *this,
                        static_cast<uapmd_track_index_t>(i),
                        *tracks_[i],
                        tp);
                    processedByExtension = true;
                    break;
                }
            }
            if (!processedByExtension && !tracks_[i]->bypassed())
                tracks_[i]->graph().processAudio(tp);
            else if (!processedByExtension)
                tp.clearAudioOutputs();

            if (eventHandlers)
                for (auto* handler : *eventHandlers)
                    if (handler)
                        handler->afterTrackProcess(event);
            tp.eventIn().position(0); // reset

            // Clear processing flag AFTER we're done with the track context
            track_processing_flags_[i]->store(false, std::memory_order_release);
        }

#ifdef __EMSCRIPTEN__
        publishWebAudioTrackCount(static_cast<uint32_t>(processTrackCount));
#endif

        for (size_t i = 0; i < sequence.tracks.size() && i < tracks_.size(); ++i) {
            auto* track = tracks_[i].get();
            auto* ctx = sequence.tracks[i];
            if (!track || !ctx)
                continue;
#ifdef __EMSCRIPTEN__
            // Publish the latency-aligned signal. The AudioWorklet replaces this
            // exact dry contribution in the native master mix with WebCLAP DSP.
            publishWebAudioTrackOutput(static_cast<uint32_t>(i), *ctx);
#endif
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

        auto* masterCtx = master_track_context_.get();
        if (master_track_ && masterCtx) {
            masterCtx->frameCount(trackFrameCount);
            masterCtx->eventIn().position(0);
            masterCtx->eventOut().position(0);
            clearAudioInputBuses(*masterCtx);
            masterCtx->clearAudioOutputs();
        }

        // Solo is additive at the engine level: any number of tracks may be
        // marked solo, and only soloed tracks are admitted when at least one is
        // selected. Mute always wins. Gate after graph processing so plugin
        // state, tails, meters, and timeline state remain continuous.
        bool anySolo = false;
        for (const auto& track : tracks_)
            if (track && track->solo()) {
                anySolo = true;
                break;
            }

        // Stage compensated track output buses into a dedicated mixer context so
        // downstream processing can still see per-bus structure before the final
        // master/device fold.
        for (uint32_t t = 0, nTracks = tracks_.size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto* track = tracks_[t].get();
            auto ctx = data.tracks[t];
            if (!track || !ctx)
                continue;
            ctx->eventIn().position(0); // clean up *in* events here.
            if (track->muted() || (anySolo && !track->solo()))
                continue;

            for (uint32_t busIndex = 0; busIndex < ctx->audioOutBusCount(); ++busIndex) {
                const auto target = effectiveTrackOutputBusRoutingTarget(static_cast<uapmd_track_index_t>(t), busIndex);
                switch (target.type) {
                    case TrackOutputRoutingTargetType::MASTER_INPUT_BUS:
                        if (masterCtx)
                            accumulateAudioBusToInput(*masterCtx, target.bus_index, *ctx, busIndex, trackFrameCount);
                        break;
                    case TrackOutputRoutingTargetType::MAIN_MIX_BUS:
                        if (mixCtx)
                            accumulateAudioBus(*mixCtx, target.bus_index, *ctx, busIndex, trackFrameCount);
                        break;
                    case TrackOutputRoutingTargetType::DISABLED:
                    default:
                        break;
                }
            }
        }

        // Return consumed pump slots to the free queue so the pump can reuse them.
        // Done after the mixing loop so no slot is recycled while its output buffers
        // are still being read.
        for (size_t t = 0; t < tracks_.size() && t < pump_rings_.size(); t++)
            if (rt_dequeued_slots_[t] != SIZE_MAX)
                pump_rings_[t]->free_slots.try_enqueue(rt_dequeued_slots_[t]);

        // Route the mix through the master track graph unconditionally so that the
        // master GainNode (always present) applies the master volume even when no
        // plugins have been added to the master track.
        if (master_track_ && master_track_context_) {
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

        // The analyser nodes retain a lock-free snapshot for the UI and for any
        // other non-audio-thread consumers.
        float outputPeak = 0.0f;
        if (process.audioOutBusCount() > 0)
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ++ch) {
                const auto* buffer = process.getFloatOutBuffer(0, ch);
                if (!buffer)
                    continue;
                for (uint32_t frame = 0; frame < process.frameCount(); ++frame)
                    outputPeak = std::max(outputPeak, std::abs(buffer[frame]));
            }

        // Muted drain: silence the device output *after* the spectrum was computed so
        // the shutdown sequence can still observe how much tail audio remains.
        const bool silenceStoppedFreezeOutput =
            tail_process_manager_->shouldSilenceStoppedOutput() &&
            !isPlaybackActive;
        if ((output_muted_.load(std::memory_order_acquire) ||
             silenceStoppedFreezeOutput) &&
            process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++)
                memset(process.getFloatOutBuffer(0, ch), 0, process.frameCount() * sizeof(float));
        }

        if (isPlaybackActive) {
            render_playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);
            if (!prerollActive)
                playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);
        }
        tail_process_manager_->processAudio(
            outputPeak, process.frameCount());

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

    bool SequencerEngineImpl::beginOfflineTrackRender(
        const OfflineTrackRenderSettings& settings,
        std::string& error) {
        error.clear();
        if (track_freeze_render_session_ ||
            track_freeze_render_active_.load(std::memory_order_acquire)) {
            error = "Another track render is already active.";
            return false;
        }
        if (settings.trackIndex < 0 ||
            static_cast<size_t>(settings.trackIndex) >= tracks_.size()) {
            error = "Track index is invalid.";
            return false;
        }
        if (settings.sampleRate <= 0 || settings.bufferSize == 0 ||
            settings.umpBufferSize == 0 || settings.endSample <= settings.startSample) {
            error = "Track render settings are invalid.";
            return false;
        }
        if (isPlaybackActive()) {
            error = "Track freezing cannot start during playback.";
            return false;
        }

        auto* sourceContext = sequence.tracks[static_cast<size_t>(settings.trackIndex)];
        if (!sourceContext || sourceContext->audioOutBusCount() == 0) {
            error = "Track has no renderable output bus.";
            return false;
        }

        auto session = std::make_unique<OfflineTrackRenderSession>();
        session->settings = settings;
        session->result.startSample = settings.startSample;
        session->current_sample = settings.startSample;
        session->previous_timeline_state = timeline_->state();
        // A track render can only begin while realtime playback is inactive.
        // Do not preserve a stale caller-owned playing flag: Stop/Pause may
        // synchronously dispatch the deferred render before their caller
        // regains control.
        session->previous_timeline_state.isPlaying = false;
        session->previous_playback_position =
            session->previous_timeline_state.playheadPosition.samples;
        session->previous_offline_rendering = offlineRendering();
        session->master_context.sampleRate(settings.sampleRate);
        session->master_context.isPlaying(true);
        session->master_context.playbackPositionSamples(settings.startSample);

        session->device_context = std::make_unique<AudioProcessContext>(
            session->master_context, settings.umpBufferSize);
        session->device_context->configureMainBus(
            static_cast<int32_t>(default_input_channels_),
            static_cast<int32_t>(default_output_channels_),
            settings.bufferSize);

        session->track_context = std::make_unique<AudioProcessContext>(
            session->master_context, settings.umpBufferSize);
        session->track_context->configureMainBus(
            std::max(1, sourceContext->inputChannelCount(0)),
            std::max(1, sourceContext->outputChannelCount(0)),
            settings.bufferSize);
        session->track_context->configureAudioInputBuses(
            sourceContext->audioInputSpecs());
        session->track_context->configureAudioOutputBuses(
            sourceContext->audioOutputSpecs());

        uint64_t channelCount = 0;
        for (int32_t bus = 0;
             bus < session->track_context->audioOutBusCount();
             ++bus) {
            const auto channels =
                static_cast<uint32_t>(
                    session->track_context->outputChannelCount(bus));
            session->result.busChannelCounts.push_back(channels);
            channelCount += channels;
        }
        const auto totalFrames = static_cast<uint64_t>(
            settings.endSample - settings.startSample);
        if (channelCount == 0 ||
            totalFrames > settings.maximumBytes / sizeof(float) / channelCount) {
            error =
                "The frozen audio would exceed the per-track memory limit.";
            return false;
        }

        try {
            session->result.channels.resize(static_cast<size_t>(channelCount));
            for (auto& channel : session->result.channels)
                channel.resize(static_cast<size_t>(totalFrames), 0.0f);

            session->render_sequence.tracks.resize(
                static_cast<size_t>(settings.trackIndex) + 1, nullptr);
            session->render_sequence.tracks[
                static_cast<size_t>(settings.trackIndex)] =
                session->track_context.get();

            track_freeze_render_active_.store(
                true, std::memory_order_seq_cst);
            while (in_process_audio_.load(std::memory_order_seq_cst))
                std::this_thread::yield();
            tail_process_manager_->holdStoppedOutputSilent();

            auto* track = tracks_[static_cast<size_t>(settings.trackIndex)].get();
            for (const auto instanceId : track->orderedInstanceIds()) {
                auto* instance = getPluginInstance(instanceId);
                if (!instance)
                    continue;
                if (instance->hasUISupport() && instance->isUIVisible())
                    instance->hideUI();
                session->plugin_states.emplace_back(
                    instanceId, instance->saveStateSync());
            }

            clearTrackProcessingState(settings.trackIndex, true);
            offline_rendering_.store(true, std::memory_order_release);
            track_freeze_render_session_ = std::move(session);
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Failed to prepare existing plugin instances for rendering.";
        }

        timeline_->state() = session->previous_timeline_state;
        playbackPosition(session->previous_playback_position);
        offline_rendering_.store(
            session->previous_offline_rendering, std::memory_order_release);
        track_freeze_render_active_.store(false, std::memory_order_release);
        return false;
    }

    OfflineTrackRenderStepResult
    SequencerEngineImpl::renderOfflineTrackStep(uint32_t maximumBlocks) {
        OfflineTrackRenderStepResult step;
        auto* session = track_freeze_render_session_.get();
        if (!session) {
            step.errorMessage = "No track render is active.";
            return step;
        }
        if (maximumBlocks == 0)
            maximumBlocks = 1;

        try {
            for (uint32_t block = 0;
                 block < maximumBlocks &&
                 session->current_sample < session->settings.endSample;
                 ++block) {
                const auto frames = static_cast<int32_t>(std::min<int64_t>(
                    session->settings.endSample - session->current_sample,
                    session->settings.bufferSize));
                session->master_context.playbackPositionSamples(
                    session->current_sample);
                session->device_context->frameCount(frames);
                session->track_context->frameCount(frames);
                session->track_context->eventIn().position(0);
                session->track_context->eventOut().position(0);
                clearAudioInputBuses(*session->track_context);
                session->track_context->clearAudioOutputs();

                // TimelineFacade currently derives clip events from the
                // engine's transport. Install the render transport only for
                // this bounded call, then restore the public stopped state
                // before yielding back to the application event loop.
                timeline_->state() = session->previous_timeline_state;
                timeline_->state().loopEnabled = false;
                playbackPosition(session->current_sample);
                executing_track_freeze_render_step_ = true;
                try {
                    timeline_->processTracksAudio(
                        *session->device_context, session->render_sequence);
                } catch (...) {
                    timeline_->state() = session->previous_timeline_state;
                    playbackPosition(session->previous_playback_position);
                    executing_track_freeze_render_step_ = false;
                    throw;
                }
                timeline_->state() = session->previous_timeline_state;
                playbackPosition(session->previous_playback_position);
                tracks_[static_cast<size_t>(session->settings.trackIndex)]
                    ->graph().processAudio(*session->track_context);
                executing_track_freeze_render_step_ = false;

                size_t cachedChannel = 0;
                const auto destinationOffset =
                    static_cast<size_t>(
                        session->current_sample -
                        session->settings.startSample);
                for (int32_t bus = 0;
                     bus < session->track_context->audioOutBusCount();
                     ++bus)
                    for (uint32_t channel = 0;
                         channel < static_cast<uint32_t>(
                             session->track_context->outputChannelCount(bus));
                         ++channel) {
                        const auto* input =
                            session->track_context->getFloatOutBuffer(
                                bus, channel);
                        if (input)
                            std::copy_n(
                                input,
                                frames,
                                session->result.channels[cachedChannel].begin() +
                                    static_cast<std::ptrdiff_t>(destinationOffset));
                        ++cachedChannel;
                    }

                session->current_sample += frames;
            }
        } catch (const std::exception& error) {
            executing_track_freeze_render_step_ = false;
            step.errorMessage = error.what();
            return step;
        } catch (...) {
            executing_track_freeze_render_step_ = false;
            step.errorMessage = "Existing plugin processing failed.";
            return step;
        }

        step.progress.renderedFrames =
            session->current_sample - session->settings.startSample;
        step.progress.totalFrames =
            session->settings.endSample - session->settings.startSample;
        step.progress.renderedSeconds =
            static_cast<double>(step.progress.renderedFrames) /
            session->settings.sampleRate;
        step.progress.totalSeconds =
            static_cast<double>(step.progress.totalFrames) /
            session->settings.sampleRate;
        step.progress.progress = std::clamp(
            static_cast<double>(step.progress.renderedFrames) /
                static_cast<double>(step.progress.totalFrames),
            0.0,
            1.0);
        step.state =
            session->current_sample >= session->settings.endSample
            ? OfflineTrackRenderStepState::Complete
            : OfflineTrackRenderStepState::InProgress;
        return step;
    }

    OfflineTrackRenderResult
    SequencerEngineImpl::finishOfflineTrackRender(
        bool canceled,
        const std::function<void(OfflineTrackRenderResult&)>& transition) {
        if (!track_freeze_render_session_) {
            OfflineTrackRenderResult result;
            result.errorMessage = "No track render is active.";
            return result;
        }

        auto session = std::move(track_freeze_render_session_);
        auto& result = session->result;
        result.canceled = canceled;
        if (canceled)
            result.errorMessage = "Track render canceled.";

        auto* track =
            tracks_[static_cast<size_t>(session->settings.trackIndex)].get();
        if (track)
            for (const auto instanceId : track->orderedInstanceIds())
                if (auto* instance = getPluginInstance(instanceId))
                    instance->stopProcessing();
        try {
            for (auto& [instanceId, state] : session->plugin_states)
                if (auto* instance = getPluginInstance(instanceId))
                    instance->loadStateSync(state);
        } catch (const std::exception& exception) {
            result.errorMessage = std::format(
                "Failed to restore plugin state after freezing: {}",
                exception.what());
        } catch (...) {
            result.errorMessage =
                "Failed to restore plugin state after freezing.";
        }
        if (track)
            for (const auto instanceId : track->orderedInstanceIds())
                if (auto* instance = getPluginInstance(instanceId))
                    instance->startProcessing();

        timeline_->state() = session->previous_timeline_state;
        playbackPosition(session->previous_playback_position);
        offline_rendering_.store(
            session->previous_offline_rendering, std::memory_order_release);
        // Stop may have armed a latency drain immediately before this render.
        // The global render exclusion suspends audio callbacks, so that drain
        // must be discarded rather than allowed to resume after a long delay.
        // Clear all host-side stages while callbacks are still excluded.
        resetProcessingState();

        result.success = !result.canceled && result.errorMessage.empty();
        if (!result.success) {
            result.channels.clear();
            result.busChannelCounts.clear();
        }
        if (transition)
            transition(result);
        track_freeze_render_active_.store(false, std::memory_order_release);
        return std::move(result);
    }

    OfflineTrackRenderResult SequencerEngineImpl::renderOfflineTrack(
        const OfflineTrackRenderSettings& settings,
        const OfflineRenderCallbacks& callbacks) {
        std::string error;
        if (!beginOfflineTrackRender(settings, error)) {
            OfflineTrackRenderResult result;
            result.startSample = settings.startSample;
            result.errorMessage = std::move(error);
            return result;
        }

        bool canceled = false;
        while (true) {
            if (callbacks.shouldCancel && callbacks.shouldCancel()) {
                canceled = true;
                break;
            }
            auto step = renderOfflineTrackStep(64);
            if (callbacks.onProgress)
                callbacks.onProgress(step.progress);
            if (step.state == OfflineTrackRenderStepState::InProgress)
                continue;
            if (step.state == OfflineTrackRenderStepState::Error) {
                track_freeze_render_session_->result.errorMessage =
                    std::move(step.errorMessage);
            }
            break;
        }
        return finishOfflineTrackRender(canceled, {});
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

    void SequencerEngineImpl::setSampleRate(int32_t newSampleRate) {
        if (newSampleRate > 0) {
            sampleRate = newSampleRate;
            notifyAudioProcessingConfigurationChanged();
        }
    }

    uapmd_track_index_t SequencerEngineImpl::addEmptyTrack(
        uapmd_track_index_t insertionIndex) {
        return publishPreparedTrack(prepareTrack(), insertionIndex);
    }

    std::unique_ptr<PreparedSequencerTrack> SequencerEngineImpl::prepareTrack(
        const std::string& graphProviderId) {
        auto track = SequencerTrack::create(
            timeline_->audioGraphProviderRegistry(),
            ump_buffer_size_in_ints,
            graphProviderId);
        if (!track)
            return nullptr;
        configureTrackRouting(track.get());
        return std::make_unique<PreparedSequencerTrackImpl>(
            std::move(track), *plugin_host);
    }

    void SequencerEngineImpl::addPluginToPreparedTrack(
        PreparedSequencerTrack& prepared,
        std::string& format,
        std::string& pluginId,
        std::function<void(int32_t instanceId, std::string error)> callback,
        std::string restoreNodeId) {
        auto* preparedImpl = dynamic_cast<PreparedSequencerTrackImpl*>(&prepared);
        if (!preparedImpl || &preparedImpl->pluginHost() != plugin_host.get()) {
            callback(-1, "The prepared track belongs to another engine");
            return;
        }

        plugin_host->createPluginInstance(
            static_cast<uint32_t>(sampleRate),
            static_cast<uint32_t>(audio_buffer_size_in_frames),
            default_input_channels_,
            default_output_channels_,
            false,
            format,
            pluginId,
            [this, preparedImpl, callback = std::move(callback),
             restoreNodeId = std::move(restoreNodeId)](
                int32_t instanceId, std::string error) mutable {
                auto complete = [this, preparedImpl, callback = std::move(callback),
                                 instanceId, error = std::move(error),
                                 restoreNodeId = std::move(restoreNodeId)]() mutable {
                    if (instanceId < 0) {
                        callback(-1, "Could not create plugin: " + error);
                        return;
                    }

                    auto* instance = preparedImpl->pluginInstance(instanceId);
                    if (!instance) {
                        callback(-1, "The prepared plugin instance is unavailable");
                        return;
                    }

                    auto& track = preparedImpl->track();
                    const auto status = track.graph().appendNodeSimple(
                        instanceId,
                        instance,
                        [this, instanceId] {
                            if (auto* instance = plugin_host->getInstance(instanceId))
                                instance->bypassed(true);
                            plugin_host->deletePluginInstance(instanceId);
                        },
                        std::move(restoreNodeId));
                    if (status != 0) {
                        plugin_host->deletePluginInstance(instanceId);
                        callback(
                            -1,
                            std::format(
                                "Failed to append plugin to prepared track (status {})",
                                status));
                        return;
                    }

                    track.orderedInstanceIds().push_back(instanceId);
                    const auto autoGroup = track.findAvailableGroup();
                    if (autoGroup <= 15)
                        track.setInstanceGroup(instanceId, autoGroup);
                    callback(instanceId, {});
                };

                if (remidy::EventLoop::runningOnMainThread())
                    complete();
                else
                    remidy::EventLoop::enqueueTaskOnMainThread(std::move(complete));
            });
    }

    uapmd_track_index_t SequencerEngineImpl::publishPreparedTrack(
        std::unique_ptr<PreparedSequencerTrack> prepared,
        uapmd_track_index_t insertionIndex) {
        auto* preparedImpl = dynamic_cast<PreparedSequencerTrackImpl*>(prepared.get());
        if (!preparedImpl || &preparedImpl->pluginHost() != plugin_host.get())
            return -1;
        if (insertionIndex < 0)
            insertionIndex = static_cast<uapmd_track_index_t>(tracks_.size());
        if (static_cast<size_t>(insertionIndex) > tracks_.size())
            return -1;

        auto track = preparedImpl->releaseTrack();
        if (!track)
            return -1;
        const auto instanceIds = track->orderedInstanceIds();
        for (const auto instanceId : instanceIds)
            if (!plugin_host->getInstance(instanceId))
                return -1;

        auto trackContext = std::make_unique<AudioProcessContext>(
            sequence.masterContext(), ump_buffer_size_in_ints);
        trackContext->configureMainBus(
            default_input_channels_,
            default_output_channels_,
            audio_buffer_size_in_frames);
        for (const auto instanceId : instanceIds)
            if (auto* instance = plugin_host->getInstance(instanceId))
                ensureContextBusConfiguration(trackContext.get(), instance->audioBuses());
        applyTrackBusesLayout(track.get(), AudioGraphBusesLayout{
            static_cast<uint32_t>(trackContext->audioInBusCount()),
            static_cast<uint32_t>(trackContext->audioOutBusCount()),
            1,
            1,
        });

        auto ring = std::make_unique<PumpTrackRing>(
            sequence.masterContext(), ump_buffer_size_in_ints);
        for (auto& slot : ring->slots) {
            slot.ctx->configureMainBus(
                default_input_channels_,
                default_output_channels_,
                audio_buffer_size_in_frames);
            for (const auto instanceId : instanceIds)
                if (auto* instance = plugin_host->getInstance(instanceId))
                    ensureContextBusConfiguration(slot.ctx.get(), instance->audioBuses());
        }

        StructureMutationGuard mutationGuard(*this);
        tracks_.insert(
            tracks_.begin() + insertionIndex,
            std::move(track));
        sequence.tracks.insert(
            sequence.tracks.begin() + insertionIndex,
            trackContext.release());
        track_processing_flags_.insert(
            track_processing_flags_.begin() + insertionIndex,
            std::make_unique<std::atomic<bool>>(false));
        pump_rings_.insert(
            pump_rings_.begin() + insertionIndex,
            std::move(ring));
        pump_sequence_.tracks.insert(
            pump_sequence_.tracks.begin() + insertionIndex,
            nullptr);

        {
            std::lock_guard<std::mutex> lock(instance_map_mutex_);
            for (const auto instanceId : instanceIds)
                if (auto* instance = plugin_host->getInstance(instanceId))
                    plugin_instances_[instanceId] = instance;
        }

        for (size_t nodeIndex = 0; nodeIndex < instanceIds.size(); ++nodeIndex) {
            const auto instanceId = instanceIds[nodeIndex];
            plugin_host->onTrackGraphNodeAdded(
                instanceId,
                insertionIndex,
                false,
                static_cast<uint32_t>(nodeIndex));
            if (auto* instance = plugin_host->getInstance(instanceId)) {
                instance->bypassed(false);
                notifyPluginInstanceAdded(instanceId, *instance);
            }
        }

        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->trackAdded(insertionIndex);
        auto trackIndex = insertionIndex;

        // Keep pre-allocated work vectors in sync.
        pump_slot_indices_.resize(tracks_.size(), SIZE_MAX);
        rt_dequeued_slots_.resize(tracks_.size(), SIZE_MAX);

        // Notify timeline facade so it can create a paired TimelineTrack
        timeline_->onTrackAdded(
            default_output_channels_,
            static_cast<double>(sampleRate),
            static_cast<uint32_t>(audio_buffer_size_in_frames),
            trackIndex
        );
        refreshPlatformMidiTrackIndices();
        refreshFunctionBlockMappings();
        notifyPluginGraphChanged();
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();

        return trackIndex;
    }

    bool SequencerEngineImpl::removeTrack(uapmd_track_index_t index) {
        if (frozen_track_manager_->isTrackBusy(index))
            return false;
        if (index >= tracks_.size())
            return false;
        const auto timelineTracks = timeline_->tracks();
        const auto trackId = timelineTracks[index]->referenceId();
        removePlatformMidiTrackConnections(trackId);
        if (tracks_[index]) {
            const auto instanceIds = tracks_[index]->orderedInstanceIds();
            for (const auto instanceId : instanceIds) {
                if (auto* instance = getPluginInstance(instanceId)) {
                    if (instance->hasUISupport() && instance->isUIVisible())
                        instance->hideUI();
                    instance->destroyUI();
                }
                if (const auto fbDevice = function_block_manager.getFunctionDeviceForInstance(instanceId))
                    fbDevice->destroyDevice(instanceId);
                notifyPluginInstanceWillBeDestroyed(instanceId);
                {
                    std::lock_guard<std::mutex> lock(instance_map_mutex_);
                    plugin_instances_.erase(instanceId);
                }
            }
            function_block_manager.deleteEmptyDevices();
        }
        StructureMutationGuard mutationGuard(*this);
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
        track_processing_flags_.erase(track_processing_flags_.begin() + static_cast<long>(index));
        if (static_cast<size_t>(index) < pump_rings_.size())
            pump_rings_.erase(pump_rings_.begin() + static_cast<long>(index));
        if (static_cast<size_t>(index) < pump_sequence_.tracks.size())
            pump_sequence_.tracks.erase(pump_sequence_.tracks.begin() + static_cast<long>(index));
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->trackRemoved(index);
        pump_slot_indices_.resize(tracks_.size(), SIZE_MAX);
        rt_dequeued_slots_.resize(tracks_.size(), SIZE_MAX);
        timeline_->onTrackRemoved(static_cast<size_t>(index));
        refreshPlatformMidiTrackIndices();
        notifyPluginGraphChanged();
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();
        return true;
    }

    bool SequencerEngineImpl::replaceTrackGraph(uapmd_track_index_t trackIndex, std::unique_ptr<AudioPluginGraph>&& graph) {
        if (frozen_track_manager_->isTrackBusy(trackIndex))
            return false;
        StructureMutationGuard mutationGuard(*this);

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
        timeline_->onTrackGraphChanged(trackIndex);
        notifyPluginGraphChanged();
        return true;
    }

    void SequencerEngineImpl::addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback, std::string restoreNodeId) {
        if (frozen_track_manager_->isTrackBusy(trackIndex)) {
            callback(-1, trackIndex, "Track is busy freezing");
            return;
        }
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
                                          [this, trackIndex, targetMaster, callback,
                                           restoreNodeId = std::move(restoreNodeId)](int32_t instanceId, std::string error) mutable {
            auto complete = [this, trackIndex, targetMaster, callback, instanceId, error = std::move(error),
                             restoreNodeId = std::move(restoreNodeId)]() mutable {
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
                }, std::move(restoreNodeId));
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
                notifyPluginGraphChanged();

                // Parameter metadata change events are now handled in AudioPluginNode directly

                refreshFunctionBlockMappings();

                instance->bypassed(false);
                notifyPluginInstanceAdded(instanceId, *instance);
                reconfigureMixBusContext();
                reconfigureOutputAlignmentBuffers();
                timeline_->onTrackGraphChanged(targetMaster ? kMasterTrackIndex : trackIndex);

                callback(instanceId, targetMaster ? kMasterTrackIndex : trackIndex, "");
            };

            if (remidy::EventLoop::runningOnMainThread())
                complete();
            else
                remidy::EventLoop::enqueueTaskOnMainThread(std::move(complete));
        });
    }

    bool SequencerEngineImpl::removePluginInstance(int32_t instanceId) {
        if (!executing_track_freeze_render_step_ &&
            frozen_track_manager_->isInstanceBusy(instanceId))
            return false;
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
        // Notify before taking instance_map_mutex_: lifecycle listeners may
        // inspect the still-live instance while unregistering their observers.
        notifyPluginInstanceWillBeDestroyed(instanceId);
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
                track->removeInstance(instanceId);
                // NOTE: Empty tracks are intentionally left in place to avoid real-time safety issues.
                // They have minimal overhead (no plugins to process) and can be removed manually
                // by calling removeTrack() from a non-audio thread when appropriate.
                refreshFunctionBlockMappings();
                notifyPluginGraphChanged();
                reconfigureMixBusContext();
                reconfigureOutputAlignmentBuffers();
                timeline_->onTrackGraphChanged(static_cast<int32_t>(i));
                return true;
            }
        }
        if (master_track_ && master_track_->graph().removeNodeSimple(instanceId)) {
            master_track_->removeInstance(instanceId);
            refreshFunctionBlockMappings();
            notifyPluginGraphChanged();
            reconfigureMixBusContext();
            reconfigureOutputAlignmentBuffers();
            timeline_->onTrackGraphChanged(kMasterTrackIndex);
            return true;
        }
        return false;
    }

    void SequencerEngineImpl::removeTrack(size_t index) {
        if (index >= tracks_.size())
            return;
        const auto timelineTracks = timeline_->tracks();
        const auto trackId = timelineTracks[index]->referenceId();
        removePlatformMidiTrackConnections(trackId);
        StructureMutationGuard mutationGuard(*this);
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        if (index < sequence.tracks.size()) {
            auto* ctx = sequence.tracks[index];
            sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
            delete ctx;
        }
        track_processing_flags_.erase(track_processing_flags_.begin() + static_cast<long>(index));
        for (auto* listener : processing_lifecycle_listeners_)
            if (listener)
                listener->trackRemoved(static_cast<uapmd_track_index_t>(index));
        timeline_->onTrackRemoved(index);
        refreshPlatformMidiTrackIndices();
        notifyPluginGraphChanged();
        reconfigureMixBusContext();
        reconfigureOutputAlignmentBuffers();
    }

    // Playback control
    bool SequencerEngineImpl::isPlaybackActive() const {
        return is_playback_active_.load(std::memory_order_acquire);
    }

    void SequencerEngineImpl::playbackPosition(int64_t samples) {
        tail_process_manager_->cancelTailProcessing();
        notifyTransportTransition(
            SequencerTransportTransition::PositionChanged,
            samples);
    }

    int64_t SequencerEngineImpl::playbackPosition() const {
        return latency_compensation_manager_
            ? latency_compensation_manager_->playbackPosition()
            : playback_position_samples_.load(std::memory_order_acquire);
    }

    int64_t SequencerEngineImpl::renderPlaybackPosition() const {
        return latency_compensation_manager_
            ? latency_compensation_manager_->renderPlaybackPosition()
            : render_playback_position_samples_.load(std::memory_order_acquire);
    }

    void SequencerEngineImpl::requestAllNotesOff() {
        auto flushTrackNotes = [](SequencerTrack* track) {
            if (!track)
                return;
            for (auto& entry : track->graph().plugins())
                if (entry.second)
                    entry.second->requestStopFlush();
        };
        for (auto& track : tracks_)
            flushTrackNotes(track.get());
        flushTrackNotes(master_track_.get());
    }

    void SequencerEngineImpl::jumpPlayback(double positionSeconds) {
        if (!std::isfinite(positionSeconds))
            return;
        if (positionSeconds < 0.0) {
            std::cerr << "Warning: Negative playback jump position " << positionSeconds
                      << " seconds; clamping to 0." << std::endl;
            positionSeconds = 0.0;
        }

        const auto samples = static_cast<int64_t>(std::llround(
            positionSeconds * static_cast<double>(sampleRate)));
        requestAllNotesOff();
        playbackPosition(samples);
        transport_generation_.fetch_add(1, std::memory_order_release);
    }

    void uapmd::SequencerEngineImpl::startPlayback() {
        if (frozen_track_manager_->requestPlaybackAfterBusyTrackRestored(
                [this] { startPlayback(); }))
            return;
        tail_process_manager_->transportStarted();
        frozen_track_manager_->transportPlaybackStarted();
        notifyTransportTransition(SequencerTransportTransition::Started, 0);
        is_playback_active_.store(true, std::memory_order_release);
        timeline_->state().isPlaying = true;
        for (auto* extension : playback_engine_extensions_)
            if (extension)
                extension->playbackStarted();
    }

    void uapmd::SequencerEngineImpl::stopPlayback() {
        for (auto* extension : playback_engine_extensions_)
            if (extension)
                extension->playbackStopped();
        is_playback_active_.store(false, std::memory_order_release);
        // Establish the final public Stop position before tail draining. The
        // later transport-quiet event may start a deferred render, whose
        // snapshot must never restore the pre-Stop position or playing state.
        timeline_->state().isPlaying = false;
        timeline_->state().playheadPosition.samples = 0;
        timeline_->state().playheadPosition.legacy_beats = 0.0;
        notifyTransportTransition(SequencerTransportTransition::Stopped, 0);
        tail_process_manager_->beginStoppedTransport(
            latency_compensation_manager_
                ? latency_compensation_manager_->stopDrainInSamples()
                : 0);
        requestAllNotesOff();
        frozen_track_manager_->transportPlaybackStopped();
    }

    void uapmd::SequencerEngineImpl::pausePlayback() {
        is_playback_active_.store(false, std::memory_order_release);
        timeline_->state().isPlaying = false;
        notifyTransportTransition(
            SequencerTransportTransition::Paused,
            playback_position_samples_.load(std::memory_order_acquire));
        tail_process_manager_->beginStoppedTransport(
            latency_compensation_manager_
                ? latency_compensation_manager_->stopDrainInSamples()
                : 0);
        requestAllNotesOff();
        frozen_track_manager_->transportPlaybackStopped();
    }

    void uapmd::SequencerEngineImpl::resumePlayback() {
        if (frozen_track_manager_->requestPlaybackAfterBusyTrackRestored(
                [this] { resumePlayback(); }))
            return;
        tail_process_manager_->transportStarted();
        frozen_track_manager_->transportPlaybackStarted();
        notifyTransportTransition(
            SequencerTransportTransition::Resumed,
            playback_position_samples_.load(std::memory_order_acquire));
        is_playback_active_.store(true, std::memory_order_release);
        timeline_->state().isPlaying = true;
    }

    webaudio_compat::AnalyserNode* SequencerEngineImpl::inputAnalyser() {
        return input_analyser_.get();
    }

    webaudio_compat::AnalyserNode* SequencerEngineImpl::outputAnalyser() {
        return output_analyser_;
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

                // FIXME: we have to strictly determine whether the output event handler must be RT-safe or not.
                // Defer the node lookup and notification to the main thread. This
                // callback runs on the audio thread, where getPluginNode() is both
                // forbidden (it takes the graph's non-realtime farbot access, a
                // blocking mutex) and deadlock-prone: the UI thread holds that mutex
                // while spin-waiting for the audio thread in nonRealtimeRelease().
                // The parameter listeners are UI/JS code that expects the main
                // thread anyway (all other notify sites run there).
                // FIXME: enqueueTaskOnMainThread() allocates and briefly locks the
                // task queue mutex, so this path is not strictly lock-free; NRPN
                // output events are sporadic enough that this is acceptable for now.
                remidy::EventLoop::enqueueTaskOnMainThread([this, instanceId, paramId, value] {
                    for (const auto& track : tracks()) {
                        if (auto* node = track->graph().getPluginNode(instanceId)) {
                            node->parameterUpdateEvent().notify(paramId, value);
                            return;
                        }
                    }
                    if (master_track_) {
                        if (auto* node = master_track_->graph().getPluginNode(instanceId)) {
                            node->parameterUpdateEvent().notify(paramId, value);
                        }
                    }
                });
            }

            // Rewrite group field
            words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group) << 24);
            enqueuePlatformMidiOutput(findTrackIndexForInstance(instanceId), words, size);
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
        if (!executing_track_freeze_render_step_ &&
            frozen_track_manager_->isInstanceBusy(instanceId))
            return;
        auto scheduleForTrack = [&](SequencerTrack* track) {
            if (!track)
                return;
            const auto node = track->graph().getPluginNode(instanceId);
            if (!node)
                return;

            uint8_t group = 0xFF;
            if (const auto fb = function_block_manager.getFunctionDeviceByInstanceId(instanceId))
                group = fb->group();
            else
                group = track->getInstanceGroup(instanceId);

            if (group > 15) {
                node->scheduleEvents(timestamp, ump, sizeInBytes);
                return;
            }

            std::vector<uapmd_ump_t> routedWords((sizeInBytes + sizeof(uapmd_ump_t) - 1) / sizeof(uapmd_ump_t));
            std::memcpy(routedWords.data(), ump, sizeInBytes);

            auto* bytes = reinterpret_cast<uint8_t*>(routedWords.data());
            size_t offset = 0;
            while (offset + sizeof(uint32_t) <= sizeInBytes) {
                auto* words = reinterpret_cast<uint32_t*>(bytes + offset);
                const auto messageType = static_cast<uint8_t>(words[0] >> 28);
                const auto wordCount = umppi::umpSizeInInts(messageType);
                const auto messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
                if (messageSize == 0 || offset + messageSize > sizeInBytes)
                    break;
                words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group) << 24);
                offset += messageSize;
            }
            node->scheduleEvents(timestamp, routedWords.data(), sizeInBytes);
        };

        for (const auto& track : tracks())
            scheduleForTrack(track);
        scheduleForTrack(master_track_.get());
    }

    bool SequencerEngineImpl::connectPlatformMidiInputToTrack(
        std::string portId, ProjectObjectId trackId) {
        if (portId.empty() || trackId.empty() || timeline_->trackIndexForReferenceId(trackId) < 0)
            return false;
        auto routes = std::atomic_load_explicit(&platform_midi_input_routes_, std::memory_order_acquire);
        if (routes)
            for (const auto& existing : *routes)
                if (existing && existing->port_id == portId) {
                    auto targets = std::atomic_load_explicit(&existing->targets, std::memory_order_acquire);
                    if (targets)
                        for (const auto& target : *targets)
                            if (target && target->track_id == trackId)
                                return true;
                    auto nextTargets = std::make_shared<PlatformMidiTargets>(targets ? *targets : PlatformMidiTargets{});
                    auto target = std::make_shared<PlatformMidiTarget>();
                    target->track_id = std::move(trackId);
                    target->track_index.store(timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
                    nextTargets->push_back(std::move(target));
                    std::atomic_store_explicit(&existing->targets,
                                               std::shared_ptr<const PlatformMidiTargets>(nextTargets),
                                               std::memory_order_release);
                    return true;
                }
        auto route = std::make_shared<PlatformMidiRoute>();
        route->port_id = std::move(portId);
        route->owner = this;
        route->device = openLibreMidiInputPort(route->port_id);
        if (!route->device)
            return false;
        auto target = std::make_shared<PlatformMidiTarget>();
        target->track_id = std::move(trackId);
        target->track_index.store(timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
        route->targets = std::make_shared<PlatformMidiTargets>(PlatformMidiTargets{target});
        route->device->addInputHandler(platformMidiInputTrampoline, route.get());
        auto next = std::make_shared<PlatformMidiRoutes>(routes ? *routes : PlatformMidiRoutes{});
        next->push_back(std::move(route));
        std::atomic_store_explicit(&platform_midi_input_routes_,
                                   std::shared_ptr<const PlatformMidiRoutes>(next), std::memory_order_release);
        return true;
    }

    void SequencerEngineImpl::disconnectPlatformMidiInputFromTrack(std::string_view portId, std::string_view trackId) {
        const auto routes = std::atomic_load_explicit(&platform_midi_input_routes_, std::memory_order_acquire);
        if (!routes)
            return;
        auto next = std::make_shared<PlatformMidiRoutes>();
        next->reserve(routes->size());
        for (const auto& route : *routes) {
            if (route && route->port_id == portId) {
                const auto targets = std::atomic_load_explicit(&route->targets, std::memory_order_acquire);
                auto nextTargets = std::make_shared<PlatformMidiTargets>();
                if (targets)
                    for (const auto& target : *targets)
                        if (target && target->track_id != trackId)
                            nextTargets->push_back(target);
                if (nextTargets->empty()) {
                    route->device->removeInputHandler(platformMidiInputTrampoline);
                    continue;
                }
                std::atomic_store_explicit(&route->targets,
                                           std::shared_ptr<const PlatformMidiTargets>(nextTargets),
                                           std::memory_order_release);
            }
            next->push_back(route);
        }
        std::atomic_store_explicit(&platform_midi_input_routes_,
                                   std::shared_ptr<const PlatformMidiRoutes>(next), std::memory_order_release);
    }

    std::vector<MidiPortTrackConnection> SequencerEngineImpl::platformMidiInputConnections() const {
        std::vector<MidiPortTrackConnection> connections;
        const auto routes = std::atomic_load_explicit(&platform_midi_input_routes_, std::memory_order_acquire);
        if (!routes)
            return connections;
        for (const auto& route : *routes) {
            if (!route)
                continue;
            const auto targets = std::atomic_load_explicit(&route->targets, std::memory_order_acquire);
            if (targets)
                for (const auto& target : *targets)
                    if (target)
                        connections.push_back({route->port_id, target->track_id});
        }
        return connections;
    }

    void SequencerEngineImpl::clearPlatformMidiInputRoute() {
        const auto routes = std::atomic_exchange_explicit(
            &platform_midi_input_routes_, std::shared_ptr<const PlatformMidiRoutes>{}, std::memory_order_acq_rel);
        if (routes)
            for (const auto& route : *routes)
                if (route && route->device)
                    route->device->removeInputHandler(platformMidiInputTrampoline);
    }

    bool SequencerEngineImpl::connectPlatformMidiOutputToTrack(
        std::string portId, ProjectObjectId trackId) {
        if (portId.empty() || trackId.empty() || timeline_->trackIndexForReferenceId(trackId) < 0)
            return false;
        auto routes = std::atomic_load_explicit(&platform_midi_output_routes_, std::memory_order_acquire);
        if (routes)
            for (const auto& existing : *routes)
                if (existing && existing->port_id == portId) {
                    auto targets = std::atomic_load_explicit(&existing->targets, std::memory_order_acquire);
                    if (targets)
                        for (const auto& target : *targets)
                            if (target && target->track_id == trackId)
                                return true;
                    auto nextTargets = std::make_shared<PlatformMidiTargets>(targets ? *targets : PlatformMidiTargets{});
                    auto target = std::make_shared<PlatformMidiTarget>();
                    target->track_id = std::move(trackId);
                    target->track_index.store(timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
                    nextTargets->push_back(std::move(target));
                    std::atomic_store_explicit(&existing->targets,
                                               std::shared_ptr<const PlatformMidiTargets>(nextTargets),
                                               std::memory_order_release);
                    return true;
                }
        auto route = std::make_shared<PlatformMidiRoute>();
        route->port_id = std::move(portId);
        route->owner = this;
        route->device = openLibreMidiOutputPort(route->port_id);
        if (!route->device)
            return false;
        auto target = std::make_shared<PlatformMidiTarget>();
        target->track_id = std::move(trackId);
        target->track_index.store(timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
        route->targets = std::make_shared<PlatformMidiTargets>(PlatformMidiTargets{target});
        auto next = std::make_shared<PlatformMidiRoutes>(routes ? *routes : PlatformMidiRoutes{});
        next->push_back(std::move(route));
        std::atomic_store_explicit(&platform_midi_output_routes_,
                                   std::shared_ptr<const PlatformMidiRoutes>(next), std::memory_order_release);
        return true;
    }

    void SequencerEngineImpl::disconnectPlatformMidiOutputFromTrack(std::string_view portId, std::string_view trackId) {
        const auto routes = std::atomic_load_explicit(&platform_midi_output_routes_, std::memory_order_acquire);
        if (!routes)
            return;
        auto next = std::make_shared<PlatformMidiRoutes>();
        next->reserve(routes->size());
        for (const auto& route : *routes) {
            if (!route || route->port_id != portId) {
                next->push_back(route);
                continue;
            }
            const auto targets = std::atomic_load_explicit(&route->targets, std::memory_order_acquire);
            auto nextTargets = std::make_shared<PlatformMidiTargets>();
            if (targets)
                for (const auto& target : *targets)
                    if (target && target->track_id != trackId)
                        nextTargets->push_back(target);
            if (nextTargets->empty())
                continue;
            std::atomic_store_explicit(&route->targets,
                                       std::shared_ptr<const PlatformMidiTargets>(nextTargets),
                                       std::memory_order_release);
            next->push_back(route);
        }
        std::atomic_store_explicit(&platform_midi_output_routes_,
                                   std::shared_ptr<const PlatformMidiRoutes>(next), std::memory_order_release);
    }

    std::vector<MidiPortTrackConnection> SequencerEngineImpl::platformMidiOutputConnections() const {
        std::vector<MidiPortTrackConnection> connections;
        const auto routes = std::atomic_load_explicit(&platform_midi_output_routes_, std::memory_order_acquire);
        if (!routes)
            return connections;
        for (const auto& route : *routes) {
            if (!route)
                continue;
            const auto targets = std::atomic_load_explicit(&route->targets, std::memory_order_acquire);
            if (targets)
                for (const auto& target : *targets)
                    if (target)
                        connections.push_back({route->port_id, target->track_id});
        }
        return connections;
    }

    void SequencerEngineImpl::clearPlatformMidiOutputRoute() {
        std::atomic_store_explicit(&platform_midi_output_routes_,
                                   std::shared_ptr<const PlatformMidiRoutes>{}, std::memory_order_release);
    }

    void SequencerEngineImpl::removePlatformMidiTrackConnections(std::string_view trackId) {
        const auto inputConnections = platformMidiInputConnections();
        for (const auto& connection : inputConnections)
            if (connection.trackId == trackId)
                disconnectPlatformMidiInputFromTrack(connection.portId, trackId);
        const auto outputConnections = platformMidiOutputConnections();
        for (const auto& connection : outputConnections)
            if (connection.trackId == trackId)
                disconnectPlatformMidiOutputFromTrack(connection.portId, trackId);
    }

    void SequencerEngineImpl::refreshPlatformMidiTrackIndices() {
        const auto refresh = [this](const std::shared_ptr<const PlatformMidiRoutes>& routes) {
            if (!routes)
                return;
            for (const auto& route : *routes) {
                if (!route)
                    continue;
                const auto targets = std::atomic_load_explicit(&route->targets, std::memory_order_acquire);
                if (targets)
                    for (const auto& target : *targets)
                        if (target)
                            target->track_index.store(
                                timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
            }
        };
        refresh(std::atomic_load_explicit(&platform_midi_input_routes_, std::memory_order_acquire));
        refresh(std::atomic_load_explicit(&platform_midi_output_routes_, std::memory_order_acquire));
    }

    void SequencerEngineImpl::platformMidiInputTrampoline(
        void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        auto* route = static_cast<PlatformMidiRoute*>(context);
        if (!route || !route->owner)
            return;
        route->owner->deliverPlatformMidiInput(*route, ump, sizeInBytes, timestamp);
    }

    void SequencerEngineImpl::deliverPlatformMidiInput(
        PlatformMidiRoute& route, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        if (!ump || sizeInBytes == 0)
            return;
        const auto targets = std::atomic_load_explicit(&route.targets, std::memory_order_acquire);
        if (!targets)
            return;
        for (const auto& target : *targets) {
            if (!target)
                continue;
            const auto trackIndex = timeline_->trackIndexForReferenceId(target->track_id);
            target->track_index.store(trackIndex, std::memory_order_release);
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size())
                continue;
            auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
            if (!track)
                continue;
            // Recording state is owned exclusively by MidiRecorder. Transport
            // play/pause must not arm or disarm MIDI capture.
            midi_recorder_->record(target->track_id, ump, sizeInBytes, playbackPosition());
            for (const auto instanceId : track->orderedInstanceIds())
                if (const auto node = track->graph().getPluginNode(instanceId))
                    node->scheduleEvents(timestamp, ump, sizeInBytes);
        }
    }

    void SequencerEngineImpl::enqueuePlatformMidiOutput(
        int32_t trackIndex, const uapmd_ump_t* ump, size_t sizeInBytes) {
        if (!ump || sizeInBytes == 0 || sizeInBytes > sizeof(umppi::Ump))
            return;
        umppi::Ump message;
        std::memcpy(&message.int1, ump, sizeInBytes);
        const auto routes = std::atomic_load_explicit(&platform_midi_output_routes_, std::memory_order_acquire);
        if (routes)
            for (const auto& route : *routes)
                if (route) {
                    const auto targets = std::atomic_load_explicit(&route->targets, std::memory_order_acquire);
                    if (targets)
                        for (const auto& target : *targets)
                            if (target && target->track_index.load(std::memory_order_acquire) == trackIndex) {
                                route->output_queue.try_enqueue(message);
                                break;
                            }
                }
    }

    void SequencerEngineImpl::runPlatformMidiOutputWorker() {
        while (platform_midi_output_worker_running_.load(std::memory_order_acquire)) {
            const auto routes = std::atomic_load_explicit(&platform_midi_output_routes_, std::memory_order_acquire);
            bool sent = false;
            if (routes) for (const auto& route : *routes) {
                umppi::Ump message;
                if (!route || !route->output_queue.try_dequeue(message))
                    continue;
                route->device->send(&message.int1, message.getSizeInBytes(), 0);
                sent = true;
            }
            if (!sent) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void SequencerEngineImpl::sendNoteOn(int32_t instanceId, int32_t note) {
        if (!executing_track_freeze_render_step_ &&
            frozen_track_manager_->isInstanceBusy(instanceId))
            return;
        uapmd_ump_t umps[2];
        auto ump = umppi::UmpFactory::midi2NoteOn(0, 0, note, 0, 0xF800, 0);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::sendNoteOff(int32_t instanceId, int32_t note) {
        if (!executing_track_freeze_render_step_ &&
            frozen_track_manager_->isInstanceBusy(instanceId))
            return;
        uapmd_ump_t umps[2];
        auto ump = umppi::UmpFactory::midi2NoteOff(0, 0, note, 0, 0xF800, 0);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::sendPitchBend(int32_t instanceId, float normalizedValue) {
        if (!executing_track_freeze_render_step_ &&
            frozen_track_manager_->isInstanceBusy(instanceId))
            return;
        uapmd_ump_t umps[2];
        float clamped = std::clamp((normalizedValue + 1.0f) * 0.5f, 0.0f, 1.0f);
        uint32_t pitchValue = static_cast<uint32_t>(clamped * 4294967295.0f);
        auto ump = umppi::UmpFactory::midi2PitchBendDirect(0, 0, pitchValue);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::sendChannelPressure(int32_t instanceId, float pressure) {
        if (!executing_track_freeze_render_step_ &&
            frozen_track_manager_->isInstanceBusy(instanceId))
            return;
        uapmd_ump_t umps[2];
        float clamped = std::clamp(pressure, 0.0f, 1.0f);
        uint32_t pressureValue = static_cast<uint32_t>(clamped * 4294967295.0f);
        auto ump = umppi::UmpFactory::midi2CAf(0, 0, pressureValue);
        umps[0] = static_cast<uapmd_ump_t>(ump >> 32);
        umps[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
        enqueueUmp(instanceId, umps, sizeof(umps), 0);
    }

    void SequencerEngineImpl::setParameterValue(int32_t instanceId, int32_t index, double value) {
        if (!executing_track_freeze_render_step_ &&
            frozen_track_manager_->isInstanceBusy(instanceId))
            return;
        auto* instance = getPluginInstance(instanceId);
        if (!instance) {
            remidy::Logger::global()->logError(std::format("setParameterValue: invalid instance {}", instanceId).c_str());
            return;
        }
        instance->setParameterValue(index, value);
        notifyTrackAudioContentChanged(findTrackIndexForInstance(instanceId));
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
