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
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <unordered_set>
#include <umppi/umppi.hpp>

#include <remidy/detail/event-loop.hpp>
#include <remidy/remidy.hpp>
#include "uapmd-engine/uapmd-engine.hpp"
#include "LatencyCompensationManagerImpl.hpp"
#include "TailProcessManagerImpl.hpp"
#include "TrackRoutingManager.hpp"
#include "AudioTrackWorkerPool.hpp"
#include "readerwriterqueue.h"

#ifdef __EMSCRIPTEN__
#include "../devices/WebAudioWorkletIODevice.hpp"
#endif

using namespace uapmd_midi_service;
using namespace uapmd_graph;

namespace uapmd {

    static uint32_t defaultAudioWorkerCount() {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        return 0;
#else
        const auto cpuCount = std::thread::hardware_concurrency();
        // Unknown or low CPU concurrency still uses one desktop worker.
        return cpuCount < 4 ? 1u : cpuCount < 8 ? 2u : cpuCount / 2u;
#endif
    }

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
            RtSnapshotPublisher<PlatformMidiTargets> targets;
            SequencerEngineImpl* owner{};
            moodycamel::ReaderWriterQueue<umppi::Ump> output_queue{256};
        };
        using PlatformMidiRoutes = std::vector<std::shared_ptr<PlatformMidiRoute>>;
        RtSnapshotPublisher<PlatformMidiRoutes> platform_midi_input_routes_;
        // Reader 0 is the audio callback; reader 1 is the MIDI output worker.
        RtSnapshotPublisher<PlatformMidiRoutes, 2> platform_midi_output_routes_;
        std::unique_ptr<MidiRecorder> midi_recorder_;
        std::vector<PlaybackEngineExtension*> playback_engine_extensions_;
        std::atomic<bool> platform_midi_output_worker_running_{true};
        std::thread platform_midi_output_worker_;
        struct PluginControlNotification {
            int32_t instance_id;
            int32_t parameter_id;
            double value;
            bool preset_request{};
            uint64_t generation{};
        };
        struct PluginControlDispatch {
            // Producer: audio coordinator; consumer: main event loop. The
            // dispatch worker only reads the atomic scheduling flags.
            moodycamel::ReaderWriterQueue<PluginControlNotification> queue{1024};
            std::atomic<bool> ready{false};
            std::atomic<bool> pending{false};
            std::atomic<uint32_t> dropped{0};
            std::atomic<uint32_t> dropped_presets{0};
            // Main-thread only, including engine destruction. Posted tasks own
            // this state independently and become no-ops after owner is cleared.
            SequencerEngineImpl* owner{};
        };
        std::shared_ptr<PluginControlDispatch> plugin_control_dispatch_ =
            std::make_shared<PluginControlDispatch>();
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

        static_assert(std::atomic<uint32_t>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);
        std::atomic<bool> audio_processing_timing_enabled_{false};
        moodycamel::ReaderWriterQueue<AudioProcessingTiming> audio_processing_timings_{4096};
        std::atomic<uint32_t> realtime_block_count_{0};
        std::atomic<uint32_t> audio_deadline_misses_{0};
        std::atomic<uint32_t> dropped_timing_records_{0};
        uint64_t timing_block_number_{0}; // processing thread only

        // Plugin instance management
        std::unordered_map<int32_t, AudioPluginInstanceAPI*> plugin_instances_;
        std::mutex instance_map_mutex_;
        mutable std::mutex dirty_state_mutex_;
        std::unordered_set<std::string> dirty_track_reference_ids_;
        bool master_track_dirty_{false};


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
        RtSnapshotPublisher<TrackAudioProcessorExtensions> track_audio_processor_extensions_;
        using AudioProcessingEventHandlers = std::vector<AudioProcessingEventHandler*>;
        RtSnapshotPublisher<AudioProcessingEventHandlers> audio_processing_event_handlers_;
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
        uint32_t structure_mutation_depth_{}; // serialized control thread only
        std::unique_ptr<AudioTrackWorkerPool> audio_workers_;
        AudioWorkerThreadSetup audio_worker_thread_setup_;
        std::vector<AudioTrackJob> audio_track_jobs_;
        std::atomic<AudioWorkerFault> audio_worker_fault_{AudioWorkerFault::None};
        std::atomic<bool> stop_on_audio_worker_deadline_{false};
        static_assert(std::atomic<AudioWorkerFault>::is_always_lock_free);

        // Callback-owned until the batch retires. Control access requires the
        // mutation guard or stopped device callbacks, just like the pump slots.
        bool audio_worker_batch_pending_{};
        AudioWorkerFaultDiagnostic audio_worker_diagnostic_;
        // Repeated recoverable overruns need separate immutable reports: logging
        // may still be reading an earlier incident when the next batch starts.
        // Producer roles migrate only while callbacks are excluded. Consumers
        // are serialized by the non-RT mutex. try_enqueue never grows the queue.
        moodycamel::ReaderWriterQueue<AudioWorkerFaultDiagnostic> audio_worker_diagnostics_{32};
        std::atomic<uint32_t> dropped_audio_worker_diagnostics_{0};
        AudioWorkerFaultDiagnostic last_audio_worker_diagnostic_;
        std::mutex audio_worker_diagnostic_mutex_;
        void publishAudioWorkerDiagnostic() {
            if (!audio_worker_diagnostics_.try_enqueue(audio_worker_diagnostic_))
                dropped_audio_worker_diagnostics_.fetch_add(1, std::memory_order_relaxed);
        }
        void retireLateAudioWorkerBatch();
        void reportAudioWorkerFault();

        void waitForAudioWorkers() override {
            if (audio_workers_)
                audio_workers_->waitUntilIdle();
            retireLateAudioWorkerBatch();
            audioWorkerDiagnostic();
        }

        // Control-thread only, with nesting for lifecycle/transport notifications.
        // A faulted callback may have returned while workers still own contexts;
        // wait for those participants as well before mutating any shared storage.
        struct StructureMutationGuard {
            SequencerEngineImpl& engine;
            explicit StructureMutationGuard(SequencerEngineImpl& e) : engine(e) {
                if (engine.structure_mutation_depth_++ != 0)
                    return;
                engine.structure_mutation_active_.store(true, std::memory_order_seq_cst);
                while (engine.in_process_audio_.load(std::memory_order_seq_cst))
                    std::this_thread::yield();
                engine.waitForAudioWorkers();
            }
            ~StructureMutationGuard() {
                if (--engine.structure_mutation_depth_ == 0)
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
            // Audio graph providers are contributed, not replaced: an addin
            // adds its own graph implementation alongside the built-in ones.
            // The registry stays owned by the timeline facade; an addin must
            // remove whatever it added during cleanup().
            manager.registerExtensionPoint(
                "/uapmd/audio-graph/provider/v1",
                &timeline_->audioGraphProviderRegistry());
        }

        explicit SequencerEngineImpl(
            int32_t sampleRate,
            size_t audioBufferSizeInFrames,
            size_t umpBufferSizeInInts,
            std::unique_ptr<AudioPluginHostingAPI> suppliedPluginHost = {},
            ProjectHistoryFactory historyFactory = {});
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
            StructureMutationGuard guard(*this);
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
            const auto* current = track_audio_processor_extensions_.currentOnPublisherThread();
            if (std::find(current->begin(), current->end(), &extension) != current->end())
                return;
            auto updated = std::make_unique<TrackAudioProcessorExtensions>(*current);
            updated->push_back(&extension);
            track_audio_processor_extensions_.publish(std::move(updated));
        }
        void removeTrackAudioProcessorExtension(TrackAudioProcessorExtension& extension) override {
            const auto* current = track_audio_processor_extensions_.currentOnPublisherThread();
            auto updated = std::make_unique<TrackAudioProcessorExtensions>(*current);
            std::erase(*updated, &extension);
            track_audio_processor_extensions_.publish(std::move(updated));
        }
        void addAudioProcessingEventHandler(AudioProcessingEventHandler& handler) override {
            const auto* current = audio_processing_event_handlers_.currentOnPublisherThread();
            if (std::find(current->begin(), current->end(), &handler) != current->end())
                return;
            auto updated = std::make_unique<AudioProcessingEventHandlers>(*current);
            updated->push_back(&handler);
            audio_processing_event_handlers_.publish(std::move(updated));
        }
        void removeAudioProcessingEventHandler(AudioProcessingEventHandler& handler) override {
            const auto* current = audio_processing_event_handlers_.currentOnPublisherThread();
            auto updated = std::make_unique<AudioProcessingEventHandlers>(*current);
            std::erase(*updated, &handler);
            audio_processing_event_handlers_.publish(std::move(updated));
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
            const auto* extensions = track_audio_processor_extensions_.currentOnPublisherThread();
            for (auto* extension : *extensions)
                if (extension)
                    extension->audioContentChanged(*this, trackIndex);
        }

        void pumpAudio(AudioProcessContext& process) override;
        uapmd_status_t processAudio(AudioProcessContext& process) override;
        bool configureAudioWorkers(uint32_t workerCount) override;
        bool setAudioWorkerThreadSetup(AudioWorkerThreadSetup setup) override {
            StructureMutationGuard guard(*this);
            const auto count = audioWorkerCount();
            audio_workers_.reset();
            audio_worker_thread_setup_ = std::move(setup);
            return configureAudioWorkers(count);
        }
        uint32_t audioWorkerCount() const override {
            return audio_workers_ ? audio_workers_->workerCount() : 0;
        }
        AudioWorkerFault audioWorkerFault() const override {
            return audio_worker_fault_.load(std::memory_order_acquire);
        }
        bool stopOnAudioWorkerDeadline() const override {
            return stop_on_audio_worker_deadline_.load(std::memory_order_relaxed);
        }
        void setStopOnAudioWorkerDeadline(bool enabled) override {
            stop_on_audio_worker_deadline_.store(enabled, std::memory_order_relaxed);
        }
        AudioWorkerFaultDiagnostic audioWorkerDiagnostic() override {
            // A latched stop prevents another batch from reusing progress storage.
            // Observe callback exit before inspecting that batch. Recoverable
            // incidents publish completion through the queue before reuse instead.
            const bool faultPublished = audio_worker_fault_.load(std::memory_order_acquire) != AudioWorkerFault::None &&
                !in_process_audio_.load(std::memory_order_acquire);
            reportAudioWorkerFault();
            std::lock_guard lock(audio_worker_diagnostic_mutex_);
            if (faultPublished && audio_workers_ && !audio_workers_->busy() &&
                last_audio_worker_diagnostic_.block_number == audio_worker_diagnostic_.block_number &&
                last_audio_worker_diagnostic_.engine_stopped &&
                !last_audio_worker_diagnostic_.completion_available) {
                last_audio_worker_diagnostic_.after_completion = audio_workers_->progressSnapshot();
                last_audio_worker_diagnostic_.completion_available = true;
            }
            return last_audio_worker_diagnostic_;
        }
        void resetAudioWorkerFault() override {
            StructureMutationGuard guard(*this);
            if (audio_worker_fault_.load(std::memory_order_acquire) == AudioWorkerFault::None)
                return;
            engine_active_.store(false, std::memory_order_release);
            resetProcessingState();
            requestAllNotesOff();
            reportAudioWorkerFault();
            audio_worker_fault_.store(AudioWorkerFault::None, std::memory_order_release);
        }
        void setAudioProcessingTimingEnabled(bool enabled) override {
            audio_processing_timing_enabled_.store(enabled, std::memory_order_relaxed);
        }
        bool tryDequeueAudioProcessingTiming(AudioProcessingTiming& timing) override {
            return audio_processing_timings_.try_dequeue(timing);
        }
        AudioProcessingTimingCounters audioProcessingTimingCounters() const override {
            return {
                realtime_block_count_.load(std::memory_order_relaxed),
                audio_deadline_misses_.load(std::memory_order_relaxed),
                dropped_timing_records_.load(std::memory_order_relaxed),
            };
        }
        uint32_t droppedPluginParameterNotificationCount() const override {
            return plugin_control_dispatch_->dropped.load(std::memory_order_relaxed);
        }
        uint32_t droppedPluginPresetRequestCount() const override {
            return plugin_control_dispatch_->dropped_presets.load(std::memory_order_relaxed);
        }
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
        ProjectCommands& commands() override { return timeline_->commands(); }

        bool isProjectDirty() const override {
            return timeline_->hasPendingPluginMutations()
                || timeline_->commands().history().state().dirty;
        }

        bool isTrackDirty(int32_t trackIndex) const override;
        void markTrackDirty(int32_t trackIndex, bool dirty) override;
        void clearTrackDirtyState() override;

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
        void dispatchTrackPluginOutput(SequencerTrack& track);
        void refreshProcessingGroups(SequencerTrack& track);
        void notifyPluginOutputParameter(const PluginControlNotification& notification);
        void applyPluginPresetRequest(const PluginControlNotification& notification);
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
        size_t umpBufferSizeInInts,
        ProjectHistoryFactory historyFactory
    ) {
        return std::make_unique<SequencerEngineImpl>(
            sampleRate,
            audioBufferSizeInFrames,
            umpBufferSizeInInts,
            nullptr,
            std::move(historyFactory));
    }

    std::unique_ptr<SequencerEngine> SequencerEngine::createWithPluginHost(
        int32_t sampleRate,
        size_t audioBufferSizeInFrames,
        size_t umpBufferSizeInInts,
        std::unique_ptr<AudioPluginHostingAPI> pluginHost,
        ProjectHistoryFactory historyFactory) {
        return std::make_unique<SequencerEngineImpl>(
            sampleRate,
            audioBufferSizeInFrames,
            umpBufferSizeInInts,
            std::move(pluginHost),
            std::move(historyFactory));
    }

    // SequencerEngineImpl
    SequencerEngineImpl::SequencerEngineImpl(
        int32_t sampleRate,
        size_t audioBufferSizeInFrames,
        size_t umpBufferSizeInInts,
        std::unique_ptr<AudioPluginHostingAPI> suppliedPluginHost,
        ProjectHistoryFactory historyFactory) :
        audio_buffer_size_in_frames(audioBufferSizeInFrames),
        sampleRate(sampleRate),
        ump_buffer_size_in_ints(umpBufferSizeInInts),
        plugin_host(suppliedPluginHost
            ? std::move(suppliedPluginHost)
            : AudioPluginHostingAPI::create()) {
        input_analyser_ = webaudio_compat::createAnalyserNode({.node_id = "engine-input-analyser"});
        timeline_ = TimelineFacade::create(*this, std::move(historyFactory));
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
        plugin_control_dispatch_->owner = this;
        platform_midi_output_worker_ = std::thread([this] { runPlatformMidiOutputWorker(); });

        // Call the pump-aware overload so that processTracksAudio writes into
        // pump_sequence_.tracks[i] (ring-buffer slots) instead of sequence.tracks[i].
        audio_preprocess_callback_ = [this](AudioProcessContext& process) {
            timeline_->processTracksAudio(process, pump_sequence_);
        };
        notifyAudioProcessingConfigurationChanged();
        configureAudioWorkers(defaultAudioWorkerCount());
    }

    SequencerEngineImpl::~SequencerEngineImpl() {
        waitForAudioWorkers();
        audio_workers_.reset();
        plugin_control_dispatch_->owner = nullptr;
        clearPlatformMidiInputRoute();
        clearPlatformMidiOutputRoute();
        platform_midi_output_worker_running_.store(false, std::memory_order_release);
        if (platform_midi_output_worker_.joinable())
            platform_midi_output_worker_.join();
        reportAudioWorkerFault();
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
        waitForAudioWorkers();
        transport_generation_.fetch_add(1, std::memory_order_acq_rel);
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
            track->clearPluginOutputEvents();
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
        if (tracks_[index])
            tracks_[index]->clearPluginOutputEvents();
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

    bool SequencerEngineImpl::configureAudioWorkers(uint32_t workerCount) {
        if (workerCount > 32)
            return false;
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        if (workerCount != 0)
            return false;
#endif
        StructureMutationGuard guard(*this);
        try {
            auto workers = workerCount ? std::make_unique<AudioTrackWorkerPool>(workerCount, audio_worker_thread_setup_) : nullptr;
            audio_track_jobs_.resize(tracks_.size());
            audio_workers_ = std::move(workers);
            if (workerCount)
                remidy::Logger::global()->logInfo("Audio workers configured: %u; platform thread setup: %s",
                    workerCount, audio_worker_thread_setup_ ? "enabled" : "none");
            return true;
        } catch (...) {
            return false;
        }
    }

    void SequencerEngineImpl::retireLateAudioWorkerBatch() {
        if (!audio_worker_batch_pending_)
            return;
        // Caller has observed idle with acquire semantics. Never read job results
        // or recycle contexts until every admitted participant has retired.
        audio_worker_diagnostic_.after_completion = audio_workers_->progressSnapshot();
        audio_worker_diagnostic_.completion_available = true;
        for (uint32_t i = 0; i < audio_worker_diagnostic_.track_count; ++i)
            if (audio_track_jobs_[i].status != 0) {
                // A late job may also have failed. Deadline recovery must not
                // hide that error or restart a broken plugin automatically.
                audio_worker_diagnostic_.fault = AudioWorkerFault::PluginFailure;
                audio_worker_diagnostic_.failed_track = static_cast<int32_t>(i);
                audio_worker_diagnostic_.plugin_status = audio_track_jobs_[i].status;
                audio_worker_diagnostic_.engine_stopped = true;
                audio_worker_fault_.store(AudioWorkerFault::PluginFailure, std::memory_order_release);
                engine_active_.store(false, std::memory_order_release);
                break;
            }
        publishAudioWorkerDiagnostic();
        for (auto& flag : track_processing_flags_)
            flag->store(false, std::memory_order_release);
        // Discard the late block, including events, without processing any job
        // twice. No post-processing handler sees an abandoned block.
        for (auto* context : sequence.tracks) {
            context->eventIn().position(0);
            context->eventOut().position(0);
            context->clearAudioOutputs();
        }
        for (size_t i = 0; i < pump_rings_.size() && i < rt_dequeued_slots_.size(); ++i)
            if (rt_dequeued_slots_[i] != SIZE_MAX) {
                pump_rings_[i]->free_slots.try_enqueue(rt_dequeued_slots_[i]);
                rt_dequeued_slots_[i] = SIZE_MAX;
            }
        audio_worker_batch_pending_ = false;
    }

    int32_t SequencerEngineImpl::processAudio(AudioProcessContext& process) {
        remidy::AudioThreadScope audioThreadScope;
        // Record start time for deadline tracking
        auto startTime = std::chrono::steady_clock::now();

        // Structural-mutation handshake: announce we're inside the audio walk before
        // anything touches the per-track vectors, then back out with silence if a
        // main-thread mutation is in flight (see structure_mutation_active_).
        InProcessAudioScope inProcessAudio(in_process_audio_);
        if (structure_mutation_active_.load(std::memory_order_seq_cst) ||
            track_freeze_render_active_.load(std::memory_order_seq_cst) ||
            audio_worker_fault_.load(std::memory_order_acquire) != AudioWorkerFault::None) {
            process.clearAudioOutputs();
            process.eventOut().position(0);
            return 0;
        }

        if (audio_worker_batch_pending_) {
            if (audio_workers_->busy()) {
                // Workers still own the previous block's contexts, events and
                // shared transport. Do not pump or dispatch another batch yet.
                process.clearAudioOutputs();
                process.eventOut().position(0);
                return 0;
            }
            retireLateAudioWorkerBatch();
            if (audio_worker_fault_.load(std::memory_order_acquire) != AudioWorkerFault::None) {
                process.clearAudioOutputs();
                process.eventOut().position(0);
                return 1;
            }
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

        const bool captureTiming = audio_processing_timing_enabled_.load(std::memory_order_relaxed);
        const bool offline = offline_rendering_.load(std::memory_order_acquire);
        const auto timingBlock = ++timing_block_number_;
        const auto timingSampleRate = sampleRate;
        using TimingClock = std::chrono::steady_clock;
        const auto publishTiming = [&](AudioProcessingStage stage,
                                       TimingClock::time_point begin,
                                       TimingClock::time_point end,
                                       int32_t trackIndex = -1) {
            if (!captureTiming)
                return;
            const AudioProcessingTiming timing{
                timingBlock, stage, trackIndex,
                stage == AudioProcessingStage::Track ? trackFrameCount : process.frameCount(), timingSampleRate,
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()),
                offline,
            };
            if (!audio_processing_timings_.try_enqueue(timing))
                dropped_timing_records_.fetch_add(1, std::memory_order_relaxed);
        };

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
        auto eventHandlers = audio_processing_event_handlers_.protect();
        auto extensions = track_audio_processor_extensions_.protect();
        bool parallel = audio_workers_ && processTrackCount > 1;
        for (size_t i = 0; parallel && i < processTrackCount; ++i)
            if (!tracks_[i]->graph().supportsParallelTrackProcessing())
                parallel = false;
        if (eventHandlers)
            for (const auto* handler : *eventHandlers)
                if (handler && !handler->supportsParallelTrackProcessing())
                    parallel = false;
        if (extensions)
            for (const auto* extension : *extensions)
                if (extension && !extension->supportsParallelTrackProcessing())
                    parallel = false;
        const auto workerDeadline = startTime + std::chrono::duration_cast<TimingClock::duration>(
            std::chrono::duration<double>(timingSampleRate > 0
                ? 0.8 * static_cast<double>(process.frameCount()) / timingSampleRate : 0.0));
        // If preparation already consumed the worker budget, retain the serial
        // path instead of publishing a batch that cannot meet its join deadline.
        parallel = parallel && (offline || TimingClock::now() < workerDeadline);
        const auto tracksStart = captureTiming ? TimingClock::now() : TimingClock::time_point{};
        publishTiming(AudioProcessingStage::Preparation, startTime, tracksStart);
        const auto finishTrack = [&](size_t i, TimingClock::time_point trackStart) {
            auto& tp = *sequence.tracks[i];
            const TrackAudioProcessingEvent event{
                static_cast<uapmd_track_index_t>(i), *tracks_[i], tp, trackFrameCount,
            };
            if (eventHandlers)
                for (auto* handler : *eventHandlers)
                    if (handler)
                        handler->afterTrackProcess(event);
            tp.eventIn().position(0);
            track_processing_flags_[i]->store(false, std::memory_order_release);
            if (captureTiming)
                publishTiming(AudioProcessingStage::Track, trackStart, TimingClock::now(), static_cast<int32_t>(i));
        };
        for (size_t i = 0; i < processTrackCount; i++) {
            const auto trackStart = captureTiming ? TimingClock::now() : TimingClock::time_point{};
            tracks_[i]->clearPluginOutputEvents();
            refreshProcessingGroups(*tracks_[i]);
            // Set processing flag BEFORE accessing sequence.tracks[i]
            track_processing_flags_[i]->store(true, std::memory_order_release);

            auto& tp = *sequence.tracks[i];
            const TrackAudioProcessingEvent event{
                static_cast<uapmd_track_index_t>(i),
                *tracks_[i],
                tp,
                trackFrameCount,
            };
            if (eventHandlers)
                for (auto* handler : *eventHandlers)
                    if (handler)
                        handler->beforeTrackProcess(event);
            bool processedByExtension = false;
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
            const bool processGraph = !processedByExtension && !tracks_[i]->bypassed();
            if (parallel) {
                auto& job = audio_track_jobs_[i];
                job = {processGraph ? &tracks_[i]->graph() : nullptr, &tp, 0, 0};
                if (!processGraph && !processedByExtension)
                    tp.clearAudioOutputs();
                if (captureTiming)
                    job.duration_nanoseconds = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(TimingClock::now() - trackStart).count());
                continue;
            }
            if (processGraph)
                tracks_[i]->graph().processAudio(tp);
            else if (!processedByExtension)
                tp.clearAudioOutputs();
            finishTrack(i, trackStart);
        }

        if (parallel) {
            const auto dispatchTime = TimingClock::now();
            audio_workers_->start(audio_track_jobs_.data(), static_cast<uint32_t>(processTrackCount), captureTiming);
            audio_workers_->participate(offline ? TimingClock::time_point::max() : workerDeadline);
            if (offline)
                audio_workers_->waitUntilIdle();
            const bool complete = offline || audio_workers_->completeBefore(workerDeadline);
            auto fault = complete ? AudioWorkerFault::None : AudioWorkerFault::DeadlineExceeded;
            int32_t failedTrack = -1;
            int32_t pluginStatus = 0;
            if (complete)
                for (size_t i = 0; i < processTrackCount; ++i)
                    if (audio_track_jobs_[i].status != 0) {
                        fault = AudioWorkerFault::PluginFailure;
                        failedTrack = static_cast<int32_t>(i);
                        pluginStatus = audio_track_jobs_[i].status;
                        break;
                    }
            if (fault != AudioWorkerFault::None) {
                // Do not mix, recycle slots, invoke post callbacks, or rerun jobs.
                // A recoverable deadline keeps the engine enabled. Subsequent
                // callbacks silence output until the old batch can be retired.
                const bool stopEngine = fault != AudioWorkerFault::DeadlineExceeded ||
                    stopOnAudioWorkerDeadline();
                audio_worker_batch_pending_ = true;
                if (stopEngine) {
                    audio_worker_fault_.store(fault, std::memory_order_release);
                    engine_active_.store(false, std::memory_order_release);
                }
                process.clearAudioOutputs();
                process.eventOut().position(0);
                const auto end = TimingClock::now();
                audio_worker_diagnostic_ = {
                    fault, timingBlock, audio_workers_->workerCount(),
                    static_cast<uint32_t>(processTrackCount), audio_workers_->pendingParticipants(),
                    process.frameCount(), timingSampleRate, failedTrack, pluginStatus,
                    std::chrono::duration<double, std::milli>(end - startTime).count(),
                    std::chrono::duration<double, std::milli>(dispatchTime - startTime).count(), offline,
                };
                audio_worker_diagnostic_.playback_position_samples = playback_position_samples_.load(std::memory_order_acquire);
                audio_worker_diagnostic_.at_fault = audio_workers_->progressSnapshot();
                audio_worker_diagnostic_.engine_stopped = stopEngine;
                publishAudioWorkerDiagnostic();
                if (!stopEngine && isPlaybackActive) {
                    // This timeline block was already fed to the plugins. Advance
                    // it once so recovery cannot replay its notes/automation.
                    const bool preroll = render_playback_position_samples_.load(std::memory_order_acquire) <
                        playback_position_samples_.load(std::memory_order_acquire);
                    render_playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);
                    if (!preroll)
                        playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);
                }
                if (!offline) {
                    realtime_block_count_.fetch_add(1, std::memory_order_relaxed);
                    if (timingSampleRate > 0 &&
                        std::chrono::duration<double>(end - startTime).count() >
                        static_cast<double>(process.frameCount()) / timingSampleRate)
                        audio_deadline_misses_.fetch_add(1, std::memory_order_relaxed);
                }
                publishTiming(AudioProcessingStage::Callback, startTime, end);
                return stopEngine ? 1 : 0;
            }
            for (size_t i = 0; i < processTrackCount; ++i) {
                // Track timing sums its own preparation, DSP and post work; time
                // spent on other tracks or waiting for workers is not attributed.
                const auto start = captureTiming ? TimingClock::now() -
                    std::chrono::duration_cast<TimingClock::duration>(
                        std::chrono::nanoseconds(audio_track_jobs_[i].duration_nanoseconds)) : TimingClock::time_point{};
                finishTrack(i, start);
            }
        }

        // Only the coordinator dispatches external events. Graph callbacks copy
        // into track-owned buffers, so workers never share output scratch,
        // snapshot reader slots, or the platform MIDI queues' producer role.
        for (size_t i = 0; i < processTrackCount; ++i)
            dispatchTrackPluginOutput(*tracks_[i]);

        const auto mixStart = captureTiming ? TimingClock::now() : TimingClock::time_point{};
        publishTiming(AudioProcessingStage::Tracks, tracksStart, mixStart);

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
            if (rt_dequeued_slots_[t] != SIZE_MAX) {
                pump_rings_[t]->free_slots.try_enqueue(rt_dequeued_slots_[t]);
                rt_dequeued_slots_[t] = SIZE_MAX;
            }

        // Route the mix through the master track graph unconditionally so that the
        // master GainNode (always present) applies the master volume even when no
        // plugins have been added to the master track.
        if (master_track_ && master_track_context_) {
            master_track_->clearPluginOutputEvents();
            refreshProcessingGroups(*master_track_);
            master_track_->graph().processAudio(*masterCtx);
            dispatchTrackPluginOutput(*master_track_);

            if (masterCtx->audioOutBusCount() > 0 && process.audioOutBusCount() > 0) {
                for (uint32_t busIndex = 0; busIndex < static_cast<uint32_t>(masterCtx->audioOutBusCount()); ++busIndex)
                    accumulateAudioBus(process, 0, *masterCtx, busIndex, trackFrameCount);
            }
        } else if (mixCtx && process.audioOutBusCount() > 0) {
            for (uint32_t busIndex = 0; busIndex < static_cast<uint32_t>(mixCtx->audioOutBusCount()); ++busIndex)
                accumulateAudioBus(process, 0, *mixCtx, busIndex, trackFrameCount);
        }

        const auto postProcessingStart = captureTiming ? TimingClock::now() : TimingClock::time_point{};
        publishTiming(AudioProcessingStage::MixAndMaster, mixStart, postProcessingStart);

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

        if (captureTiming)
            publishTiming(AudioProcessingStage::PostProcessing, postProcessingStart, TimingClock::now());
        const auto endTime = TimingClock::now();
        if (!offline) {
            realtime_block_count_.fetch_add(1, std::memory_order_relaxed);
            const auto elapsedSeconds = std::chrono::duration<double>(endTime - startTime).count();
            if (timingSampleRate > 0 && process.frameCount() > 0 &&
                elapsedSeconds > static_cast<double>(process.frameCount()) / timingSampleRate)
                audio_deadline_misses_.fetch_add(1, std::memory_order_relaxed);
        }
        publishTiming(AudioProcessingStage::Callback, startTime, endTime);

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
            waitForAudioWorkers();
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
        remidy::AudioThreadScope audioThreadScope;
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
                auto& renderTrack = *tracks_[static_cast<size_t>(session->settings.trackIndex)];
                renderTrack.clearPluginOutputEvents();
                refreshProcessingGroups(renderTrack);
                renderTrack.graph().processAudio(*session->track_context);
                dispatchTrackPluginOutput(renderTrack);
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
        StructureMutationGuard guard(*this);
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
        StructureMutationGuard guard(*this);
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
        audio_track_jobs_.resize(tracks_.size());
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
        StructureMutationGuard mutationGuard(*this);
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

                StructureMutationGuard mutationGuard(*this);
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
        StructureMutationGuard mutationGuard(*this);
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

    bool SequencerEngineImpl::isTrackDirty(int32_t trackIndex) const {
        if (trackIndex == kMasterTrackIndex) {
            std::lock_guard lock(dirty_state_mutex_);
            return master_track_dirty_;
        }
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks_.size()))
            return false;
        const auto timelineTracks = timeline_->tracks();
        if (trackIndex >= static_cast<int32_t>(timelineTracks.size())
            || !timelineTracks[static_cast<size_t>(trackIndex)])
            return false;
        std::lock_guard lock(dirty_state_mutex_);
        return dirty_track_reference_ids_.contains(
            timelineTracks[static_cast<size_t>(trackIndex)]->referenceId());
    }

    void SequencerEngineImpl::markTrackDirty(int32_t trackIndex, bool dirty) {
        if (trackIndex == kMasterTrackIndex) {
            {
                std::lock_guard lock(dirty_state_mutex_);
                master_track_dirty_ = dirty;
            }
            if (dirty && frozen_track_manager_)
                frozen_track_manager_->projectTrackBecameDirty(trackIndex);
            return;
        }
        if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks_.size()))
            return;
        const auto timelineTracks = timeline_->tracks();
        if (trackIndex >= static_cast<int32_t>(timelineTracks.size())
            || !timelineTracks[static_cast<size_t>(trackIndex)])
            return;
        const auto trackId = timelineTracks[static_cast<size_t>(trackIndex)]->referenceId();
        {
            std::lock_guard lock(dirty_state_mutex_);
            if (dirty)
                dirty_track_reference_ids_.insert(trackId);
            else
                dirty_track_reference_ids_.erase(trackId);
        }
        if (dirty && frozen_track_manager_)
            frozen_track_manager_->projectTrackBecameDirty(trackIndex);
    }

    void SequencerEngineImpl::clearTrackDirtyState() {
        std::lock_guard lock(dirty_state_mutex_);
        dirty_track_reference_ids_.clear();
        master_track_dirty_ = false;
    }

    void SequencerEngineImpl::removeTrack(size_t index) {
        if (index >= tracks_.size())
            return;
        const auto timelineTracks = timeline_->tracks();
        const auto trackId = timelineTracks[index]->referenceId();
        {
            std::lock_guard lock(dirty_state_mutex_);
            dirty_track_reference_ids_.erase(trackId);
        }
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
        StructureMutationGuard guard(*this);
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
        StructureMutationGuard guard(*this);
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
        StructureMutationGuard guard(*this);
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
        StructureMutationGuard guard(*this);
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
        StructureMutationGuard guard(*this);
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
        StructureMutationGuard guard(*this);
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
        track->configureProcessingGroups();
        refreshProcessingGroups(*track);
        track->graph().setGroupResolver([track](int32_t instanceId) {
            return track->processingGroup(instanceId);
        });
        track->graph().setPresetRequestCallback([track](int32_t instanceId, uint32_t index) {
            track->capturePresetRequest(instanceId, index);
        });
        track->graph().setEventOutputCallback([track](int32_t instanceId, const uapmd_ump_t* data, size_t dataSizeInBytes) {
            track->capturePluginOutput(instanceId, data, dataSizeInBytes);
        });
    }

    void SequencerEngineImpl::refreshProcessingGroups(SequencerTrack& track) {
        for (const auto instanceId : track.orderedInstanceIds()) {
            const auto fb = functionBlockManager()->getFunctionDeviceByInstanceId(instanceId);
            track.setProcessingGroup(instanceId, fb ? fb->group() : static_cast<uint8_t>(0xFF));
        }
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

    void SequencerEngineImpl::dispatchTrackPluginOutput(SequencerTrack& track) {
        for (const auto& event : track.pluginOutputEvents()) {
            if (!event.preset_request) {
                dispatchPluginOutput(event.instance_id, event.words.data(), event.size_in_bytes);
                continue;
            }
            // Offline/freeze rendering cannot depend on UI event-loop timing or
            // replay preset requests later against the live project.
            if (offline_rendering_.load(std::memory_order_acquire) || executing_track_freeze_render_step_) {
                plugin_control_dispatch_->dropped_presets.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const PluginControlNotification request{
                event.instance_id, static_cast<int32_t>(event.words[0]), 0.0, true,
                transport_generation_.load(std::memory_order_acquire),
            };
            if (plugin_control_dispatch_->queue.try_enqueue(request))
                plugin_control_dispatch_->ready.store(true, std::memory_order_release);
            else
                plugin_control_dispatch_->dropped_presets.fetch_add(1, std::memory_order_relaxed);
        }
        track.clearPluginOutputEvents();
    }

    // Coordinator only: group rewriting and NRPN parameter extraction for one UMP.
    void SequencerEngineImpl::dispatchPluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
        if (!data || bytes == 0)
            return;

        const auto fb = functionBlockManager()->getFunctionDeviceByInstanceId(instanceId);
        if (!fb)
            return;
        const auto group = fb->group();

        if (bytes > 4 * sizeof(uapmd_ump_t))
            return;

        uapmd_ump_t scratch[4]{};
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

                // The non-RT output worker posts UI tasks. The audio coordinator
                // only copies into a preallocated queue, never the event-loop
                // task queue (which allocates and takes a mutex).
                if (plugin_control_dispatch_->queue.try_enqueue({instanceId, paramId, value}))
                    plugin_control_dispatch_->ready.store(true, std::memory_order_release);
                else
                    plugin_control_dispatch_->dropped.fetch_add(1, std::memory_order_relaxed);
            }

            // Rewrite group field
            words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group) << 24);
            enqueuePlatformMidiOutput(findTrackIndexForInstance(instanceId), words, size);
            offset += size;
        }
    }

    void SequencerEngineImpl::applyPluginPresetRequest(const PluginControlNotification& notification) {
        // Main/control thread only. Hold exclusion through the synchronous load;
        // the async backend overload could outlive both this guard and the node.
        StructureMutationGuard guard(*this);
        if (notification.generation != transport_generation_.load(std::memory_order_acquire) ||
            offline_rendering_.load(std::memory_order_acquire) ||
            track_freeze_render_active_.load(std::memory_order_acquire) ||
            audio_worker_fault_.load(std::memory_order_acquire) != AudioWorkerFault::None)
            return;
        auto* instance = getPluginInstance(notification.instance_id);
        if (!instance || frozen_track_manager_->isInstanceBusy(notification.instance_id))
            return;
        try {
            const auto presets = instance->presetMetadataList();
            if (notification.parameter_id < 0 || static_cast<size_t>(notification.parameter_id) >= presets.size())
                return;
            instance->loadPreset(notification.parameter_id);
        } catch (const std::exception& error) {
            remidy::Logger::global()->logError("MIDI preset load failed: %s", error.what());
        } catch (...) {
            remidy::Logger::global()->logError("MIDI preset load failed");
        }
    }

    void SequencerEngineImpl::notifyPluginOutputParameter(const PluginControlNotification& notification) {
        // Main thread: graph lookup takes non-realtime access, and listeners can
        // invoke UI/JS code. Removed instances are intentionally ignored.
        for (const auto& track : tracks_)
            if (auto* node = track->graph().getPluginNode(notification.instance_id)) {
                node->parameterUpdateEvent().notify(notification.parameter_id, notification.value);
                return;
            }
        if (master_track_)
            if (auto* node = master_track_->graph().getPluginNode(notification.instance_id))
                node->parameterUpdateEvent().notify(notification.parameter_id, notification.value);
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
        const auto* routes = platform_midi_input_routes_.currentOnPublisherThread();
        for (const auto& existing : *routes)
            if (existing && existing->port_id == portId) {
                const auto* targets = existing->targets.currentOnPublisherThread();
                for (const auto& target : *targets)
                    if (target && target->track_id == trackId)
                        return true;
                auto nextTargets = std::make_unique<PlatformMidiTargets>(*targets);
                auto target = std::make_shared<PlatformMidiTarget>();
                target->track_id = std::move(trackId);
                target->track_index.store(timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
                nextTargets->push_back(std::move(target));
                existing->targets.publish(std::move(nextTargets));
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
        route->targets.publish(
            std::make_unique<const PlatformMidiTargets>(PlatformMidiTargets{target}));
        route->device->addInputHandler(platformMidiInputTrampoline, route.get());
        auto next = std::make_unique<PlatformMidiRoutes>(*routes);
        next->push_back(std::move(route));
        platform_midi_input_routes_.publish(std::move(next));
        return true;
    }

    void SequencerEngineImpl::disconnectPlatformMidiInputFromTrack(std::string_view portId, std::string_view trackId) {
        const auto* routes = platform_midi_input_routes_.currentOnPublisherThread();
        auto next = std::make_unique<PlatformMidiRoutes>();
        next->reserve(routes->size());
        for (const auto& route : *routes) {
            if (route && route->port_id == portId) {
                const auto* targets = route->targets.currentOnPublisherThread();
                auto nextTargets = std::make_unique<PlatformMidiTargets>();
                for (const auto& target : *targets)
                    if (target && target->track_id != trackId)
                        nextTargets->push_back(target);
                if (nextTargets->empty()) {
                    route->device->removeInputHandler(platformMidiInputTrampoline);
                    continue;
                }
                route->targets.publish(std::move(nextTargets));
            }
            next->push_back(route);
        }
        platform_midi_input_routes_.publish(std::move(next));
    }

    std::vector<MidiPortTrackConnection> SequencerEngineImpl::platformMidiInputConnections() const {
        std::vector<MidiPortTrackConnection> connections;
        const auto* routes = platform_midi_input_routes_.currentOnPublisherThread();
        for (const auto& route : *routes) {
            if (!route)
                continue;
            const auto* targets = route->targets.currentOnPublisherThread();
            for (const auto& target : *targets)
                if (target)
                    connections.push_back({route->port_id, target->track_id});
        }
        return connections;
    }

    void SequencerEngineImpl::clearPlatformMidiInputRoute() {
        const auto* routes = platform_midi_input_routes_.currentOnPublisherThread();
        for (const auto& route : *routes)
            if (route && route->device)
                route->device->removeInputHandler(platformMidiInputTrampoline);
        platform_midi_input_routes_.publish(std::make_unique<const PlatformMidiRoutes>());
    }

    bool SequencerEngineImpl::connectPlatformMidiOutputToTrack(
        std::string portId, ProjectObjectId trackId) {
        if (portId.empty() || trackId.empty() || timeline_->trackIndexForReferenceId(trackId) < 0)
            return false;
        const auto* routes = platform_midi_output_routes_.currentOnPublisherThread();
        for (const auto& existing : *routes)
            if (existing && existing->port_id == portId) {
                const auto* targets = existing->targets.currentOnPublisherThread();
                for (const auto& target : *targets)
                    if (target && target->track_id == trackId)
                        return true;
                auto nextTargets = std::make_unique<PlatformMidiTargets>(*targets);
                auto target = std::make_shared<PlatformMidiTarget>();
                target->track_id = std::move(trackId);
                target->track_index.store(timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
                nextTargets->push_back(std::move(target));
                existing->targets.publish(std::move(nextTargets));
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
        route->targets.publish(
            std::make_unique<const PlatformMidiTargets>(PlatformMidiTargets{target}));
        auto next = std::make_unique<PlatformMidiRoutes>(*routes);
        next->push_back(std::move(route));
        platform_midi_output_routes_.publish(std::move(next));
        return true;
    }

    void SequencerEngineImpl::disconnectPlatformMidiOutputFromTrack(std::string_view portId, std::string_view trackId) {
        const auto* routes = platform_midi_output_routes_.currentOnPublisherThread();
        auto next = std::make_unique<PlatformMidiRoutes>();
        next->reserve(routes->size());
        for (const auto& route : *routes) {
            if (!route || route->port_id != portId) {
                next->push_back(route);
                continue;
            }
            const auto* targets = route->targets.currentOnPublisherThread();
            auto nextTargets = std::make_unique<PlatformMidiTargets>();
            for (const auto& target : *targets)
                if (target && target->track_id != trackId)
                    nextTargets->push_back(target);
            if (nextTargets->empty())
                continue;
            route->targets.publish(std::move(nextTargets));
            next->push_back(route);
        }
        platform_midi_output_routes_.publish(std::move(next));
    }

    std::vector<MidiPortTrackConnection> SequencerEngineImpl::platformMidiOutputConnections() const {
        std::vector<MidiPortTrackConnection> connections;
        const auto* routes = platform_midi_output_routes_.currentOnPublisherThread();
        for (const auto& route : *routes) {
            if (!route)
                continue;
            const auto* targets = route->targets.currentOnPublisherThread();
            for (const auto& target : *targets)
                if (target)
                    connections.push_back({route->port_id, target->track_id});
        }
        return connections;
    }

    void SequencerEngineImpl::clearPlatformMidiOutputRoute() {
        platform_midi_output_routes_.publish(std::make_unique<const PlatformMidiRoutes>());
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
        const auto refresh = [this](const PlatformMidiRoutes* routes) {
            if (!routes)
                return;
            for (const auto& route : *routes) {
                if (!route)
                    continue;
                const auto* targets = route->targets.currentOnPublisherThread();
                for (const auto& target : *targets)
                    if (target)
                        target->track_index.store(
                            timeline_->trackIndexForReferenceId(target->track_id), std::memory_order_release);
            }
        };
        refresh(platform_midi_input_routes_.currentOnPublisherThread());
        refresh(platform_midi_output_routes_.currentOnPublisherThread());
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
        const auto targets = route.targets.protect();
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
        const auto routes = platform_midi_output_routes_.protect(0);
        if (routes)
            for (const auto& route : *routes)
                if (route) {
                    const auto targets = route->targets.protect();
                    if (targets)
                        for (const auto& target : *targets)
                            if (target && target->track_index.load(std::memory_order_acquire) == trackIndex) {
                                route->output_queue.try_enqueue(message);
                                break;
                            }
                }
    }

    void SequencerEngineImpl::reportAudioWorkerFault() {
        std::lock_guard lock(audio_worker_diagnostic_mutex_);
        AudioWorkerFaultDiagnostic diagnostic;
        // Bound each non-RT reporting pass even if incidents keep arriving.
        for (uint32_t count = 0; count < 32 && audio_worker_diagnostics_.try_dequeue(diagnostic); ++count) {
            const bool completionOnly = diagnostic.completion_available &&
                diagnostic.block_number == last_audio_worker_diagnostic_.block_number &&
                diagnostic.fault == last_audio_worker_diagnostic_.fault;
            last_audio_worker_diagnostic_ = diagnostic;
            if (completionOnly)
                continue;
            const double periodMs = diagnostic.sample_rate > 0
                ? 1000.0 * diagnostic.frame_count / diagnostic.sample_rate : 0.0;
            remidy::Logger::global()->logError(
                "%s: %s; block=%llu, mode=%s, workers=%u (+ coordinator), tracks=%u, "
                "frames=%d, sample_rate=%d Hz, callback_elapsed=%.3f ms, block_period=%.3f ms, "
                "worker_budget=%.3f ms (%s), dispatch_at=%.3f ms, "
                "pending_workers=%u, failed_track=%d (zero-based; -1=unknown), plugin_status=%d. "
                "%s Try Serial/fewer workers or a larger audio buffer if deadline overruns recur.",
                diagnostic.engine_stopped ? "Audio engine stopped" : "Audio deadline overrun",
                diagnostic.fault == AudioWorkerFault::DeadlineExceeded
                    ? "audio worker completion deadline exceeded" : "plugin processing failed",
                static_cast<unsigned long long>(diagnostic.block_number),
                diagnostic.offline ? "offline" : "realtime", diagnostic.worker_count, diagnostic.track_count,
                diagnostic.frame_count, diagnostic.sample_rate, diagnostic.elapsed_ms, periodMs,
                diagnostic.offline ? 0.0 : 0.8 * periodMs,
                diagnostic.offline ? "disabled offline" : "80% of block", diagnostic.dispatch_ms,
                diagnostic.pending_participants, diagnostic.failed_track, diagnostic.plugin_status,
                diagnostic.engine_stopped
                    ? "Output is silenced; restart the audio engine to resume."
                    : "Engine remains enabled; output is silenced until late workers finish, then processing resumes automatically.");
            uint32_t acknowledged = 0;
            for (uint32_t i = 1; i <= diagnostic.worker_count; ++i)
                if (diagnostic.at_fault.participants[i].acknowledged_ns != 0)
                    ++acknowledged;
            remidy::Logger::global()->logError(
                "Audio worker snapshot: block=%llu, playback_samples=%lld, workers_acknowledged=%u/%u, "
                "jobs_completed=%u/%u. Query get_audio_worker_diagnostics via MCP for per-worker/track details.",
                static_cast<unsigned long long>(diagnostic.block_number),
                static_cast<long long>(diagnostic.playback_position_samples), acknowledged, diagnostic.worker_count,
                diagnostic.at_fault.completed_jobs, diagnostic.track_count);
        }
        const auto dropped = dropped_audio_worker_diagnostics_.exchange(0, std::memory_order_relaxed);
        if (dropped != 0)
            remidy::Logger::global()->logError(
                "Audio worker diagnostics: %u incident/completion reports dropped because the diagnostic queue was full.",
                dropped);
    }

    void SequencerEngineImpl::runPlatformMidiOutputWorker() {
        while (platform_midi_output_worker_running_.load(std::memory_order_acquire)) {
            reportAudioWorkerFault();
            bool sent = false;
            auto& dispatch = *plugin_control_dispatch_;
            if (!dispatch.pending.load(std::memory_order_acquire) &&
                dispatch.ready.exchange(false, std::memory_order_acq_rel)) {
                dispatch.pending.store(true, std::memory_order_release);
                remidy::EventLoop::enqueueTaskOnMainThread([state = plugin_control_dispatch_] {
                    // Bound work per UI turn and allow at most one outstanding
                    // task even when the event loop is stalled.
                    PluginControlNotification notification;
                    size_t count = 0;
                    size_t presetCount = 0;
                    while (state->owner && count < 256 && presetCount < 4 && state->queue.try_dequeue(notification)) {
                        ++count;
                        if (notification.preset_request) {
                            ++presetCount;
                            state->owner->applyPluginPresetRequest(notification);
                        } else
                            state->owner->notifyPluginOutputParameter(notification);
                    }
                    if (count == 256 || presetCount == 4)
                        state->ready.store(true, std::memory_order_release);
                    state->pending.store(false, std::memory_order_release);
                });
                sent = true;
            }
            {
                const auto routes = platform_midi_output_routes_.protect(1);
                if (routes) for (const auto& route : *routes) {
                    umppi::Ump message;
                    if (!route || !route->output_queue.try_dequeue(message))
                        continue;
                    route->device->send(&message.int1, message.getSizeInBytes(), 0);
                    sent = true;
                }
            }
            if (!sent)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
        StructureMutationGuard mutationGuard(*this);
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
