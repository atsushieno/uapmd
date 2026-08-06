#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <uapmd-plugin-hosting/uapmd-plugin-hosting.hpp>
#include <uapmd-midi-service/uapmd-midi-service.hpp>
#include <uapmd-graph/uapmd-graph.hpp>
#include "TrackAudioProcessorExtension.hpp"
#include "AudioProcessingEventHandler.hpp"
#include "SequencerProcessingLifecycleListener.hpp"
#include "LatencyCompensationManager.hpp"
#include "OfflineRenderer.hpp"
#include "TimelineFacade.hpp"

namespace uapmd {
    class SequencerEngine;
    class FrozenTrackManager;
    class TailProcessManager;
    class MidiRecorder;
    class PlaybackEngineExtension;

    struct MidiPortTrackConnection {
        std::string portId;
        ProjectObjectId trackId;
    };

    // A sequence processor that works as a facade for the overall audio processing at each AudioPluginTrack.
    // It is used to enqueue input events to each audio track, to process once at a time when an audio I/O event arrives.
    // It is independent of DeviceIODispatcher, which fires `processAudio()` in its audio I/O callback.
    // It is also independent of the timed sequencer that is to deliver sequencer inputs on time.
    class SequencerEngine {
    protected:
        SequencerEngine() = default;

    public:
        virtual ~SequencerEngine() = default;

        virtual void enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) = 0;

        // Each platform endpoint has an independent route. The persisted track
        // key is TimelineTrack::referenceId(), never its mutable vector index.
        virtual bool connectPlatformMidiInputToTrack(
            std::string portId, ProjectObjectId trackId) = 0;
        virtual void disconnectPlatformMidiInputFromTrack(
            std::string_view portId, std::string_view trackId) = 0;
        virtual std::vector<MidiPortTrackConnection> platformMidiInputConnections() const = 0;
        virtual void clearPlatformMidiInputRoute() = 0;
        virtual bool connectPlatformMidiOutputToTrack(
            std::string portId, ProjectObjectId trackId) = 0;
        virtual void disconnectPlatformMidiOutputFromTrack(
            std::string_view portId, std::string_view trackId) = 0;
        virtual std::vector<MidiPortTrackConnection> platformMidiOutputConnections() const = 0;
        virtual void clearPlatformMidiOutputRoute() = 0;

        virtual uapmd_plugin_hosting::AudioPluginHostingAPI* pluginHost() = 0;
        virtual FrozenTrackManager& frozenTrackManager() = 0;
        virtual TailProcessManager& tailProcessManager() = 0;
        virtual uapmd_plugin_hosting::AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) = 0;
        virtual uapmd_midi_service::UapmdFunctionBlockManager* functionBlockManager() = 0;
        // FIXME: we should probably remove this at some stage
        virtual int32_t findTrackIndexForInstance(int32_t instanceId) const = 0;

        virtual std::vector<SequencerTrack *> & tracks() const = 0;
        virtual SequencerTrack* masterTrack() = 0;
        virtual size_t umpBufferSizeInBytes() const = 0;
        virtual uint32_t trackLatencyInSamples(uapmd_track_index_t trackIndex) = 0;
        virtual uint32_t masterTrackLatencyInSamples() = 0;
        virtual uint32_t trackRenderLeadInSamples(uapmd_track_index_t trackIndex) = 0;
        virtual uint32_t masterTrackRenderLeadInSamples() = 0;
        virtual bool trackHasLiveInput(uapmd_track_index_t trackIndex) = 0;
        virtual LatencyCompensationManager* latencyCompensationManager() = 0;
        virtual uint32_t trackOutputAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex) = 0;
        virtual uint32_t trackOutputBusAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) = 0;
        virtual uapmd_graph::TrackOutputRoutingTarget trackOutputBusRoutingTarget(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) = 0;
        virtual std::vector<uapmd_graph::TrackOutputRoutingRule> trackOutputRoutingRules(uapmd_track_index_t trackIndex) = 0;
        virtual void setTrackOutputRoutingRules(
            uapmd_track_index_t trackIndex,
            const std::vector<uapmd_graph::TrackOutputRoutingRule>& rules) = 0;
        virtual bool isOutputAlignmentActive() = 0;
        // Create track with plugin + configure bus (replaces manual addSimpleTrack + configureMainBus pattern)
        virtual uapmd_track_index_t addEmptyTrack() = 0;
        // Add plugin to existing track
        virtual void addPluginToTrack(uapmd_track_index_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, uapmd_track_index_t trackIndex, std::string error)> callback) = 0;
        virtual bool removePluginInstance(int32_t instanceId) = 0;
        virtual bool removeTrack(uapmd_track_index_t trackIndex) = 0;
        virtual bool replaceTrackGraph(uapmd_track_index_t trackIndex, std::unique_ptr<uapmd_graph::AudioPluginGraph>&& graph) = 0;

        // UMP group assignment helpers — search all tracks for the given instanceId.
        // getInstanceGroup returns 0xFF if the instance is not found.
        // setInstanceGroup returns false if the requested group is already taken on that track.
        virtual uint8_t getInstanceGroup(int32_t instanceId) const = 0;
        virtual bool    setInstanceGroup(int32_t instanceId, uint8_t group) = 0;
        // Clean up empty tracks (must be called from non-audio thread)
        virtual void cleanupEmptyTracks() = 0;

        // Set default channel configuration (called by RealtimeSequencer when device changes)
        virtual void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) = 0;
        virtual void setSampleRate(int32_t sampleRate) = 0;
        virtual bool offlineRendering() const = 0;
        virtual void offlineRendering(bool enabled) = 0;

        virtual void setEngineActive(bool active) = 0;

        // When muted, the graph keeps processing (so plugin release/reverb tails can
        // render out and the output spectrum stays observable) but the device output
        // is silenced. Used for the inaudible drain phase of the engine-off sequence.
        virtual void setOutputMuted(bool muted) = 0;

        // Clear all intermediate processing buffers: pump ring slots, track/mix/master
        // contexts, output alignment delay lines, tail process state, spectra, and
        // queued plugin-node events. Must only be called while the audio callback is
        // stopped (typically right after the audio engine has been turned off), so that
        // a later engine restart does not resume stale audio or replay stale events.
        virtual void resetProcessingState() = 0;
        // Clears one track's host-side buffers. When resetPlugins is true, also
        // cycles its plugin processing state; call that form from the main thread.
        // transition runs synchronously while the audio callback is excluded from
        // the track state change.
        virtual void resetTrackProcessingState(
            uapmd_track_index_t trackIndex,
            bool resetPlugins,
            const std::function<void()>& transition = {}) = 0;

        // Audio preprocessing callback (called before track processing)
        using AudioPreprocessCallback = std::function<void(AudioProcessContext& process)>;
        virtual void setAudioPreprocessCallback(AudioPreprocessCallback callback) = 0;

        virtual SequenceProcessContext& data() = 0;

        // Pump step: advance the timeline, fill per-track audio/event input buffers from
        // clip source nodes, and run the audio-preprocess callback. Intended to run on the
        // non-RT main pthread (or a dedicated pump pthread). Must complete before the
        // matching processAudio() call consumes the filled buffers.
        // For single-threaded operation, processAudio() calls this automatically.
        virtual void pumpAudio(AudioProcessContext& process) = 0;

        // The engine does not own registered extensions. Callers must unregister an
        // extension before destroying it.
        virtual void addTrackAudioProcessorExtension(TrackAudioProcessorExtension& extension) = 0;
        virtual void removeTrackAudioProcessorExtension(TrackAudioProcessorExtension& extension) = 0;
        virtual void addAudioProcessingEventHandler(AudioProcessingEventHandler& handler) = 0;
        virtual void removeAudioProcessingEventHandler(AudioProcessingEventHandler& handler) = 0;
        virtual void addProcessingLifecycleListener(SequencerProcessingLifecycleListener& listener) = 0;
        virtual void removeProcessingLifecycleListener(SequencerProcessingLifecycleListener& listener) = 0;
        virtual void addPlaybackEngineExtension(PlaybackEngineExtension& extension) = 0;
        virtual void removePlaybackEngineExtension(PlaybackEngineExtension& extension) = 0;
        virtual PlaybackEngineExtension* findPlaybackEngineExtension(std::string_view extensionId) = 0;
        virtual void notifyRecordingStarted() = 0;
        virtual void notifyRecordingStopped() = 0;

        // RT plugin chain: calls AudioPluginGraph::processAudio() for every track, mixes
        // outputs, and runs the master track. In single-threaded builds this is called
        // after pumpAudio().
        virtual uapmd_status_t processAudio(AudioProcessContext& process) = 0;
        // Existing-instance track rendering. begin excludes the realtime audio
        // callback; step performs bounded work on the calling thread; finish
        // restores plugin/transport state and invokes transition before audio
        // processing is admitted again.
        virtual bool beginOfflineTrackRender(
            const OfflineTrackRenderSettings& settings,
            std::string& error) = 0;
        virtual OfflineTrackRenderStepResult renderOfflineTrackStep(
            uint32_t maximumBlocks) = 0;
        virtual OfflineTrackRenderResult finishOfflineTrackRender(
            bool canceled,
            const std::function<void(OfflineTrackRenderResult&)>& transition = {}) = 0;
        // Synchronous convenience wrapper, primarily for non-interactive tools.
        virtual OfflineTrackRenderResult renderOfflineTrack(
            const OfflineTrackRenderSettings& settings,
            const OfflineRenderCallbacks& callbacks = {}) = 0;

        // Playback control (accessed by RealtimeSequencer)
        virtual bool isPlaybackActive() const = 0;
        virtual void playbackPosition(int64_t samples) = 0;
        virtual int64_t playbackPosition() const = 0;
        virtual int32_t currentSampleRate() const = 0;
        virtual int64_t renderPlaybackPosition() const = 0;
        virtual void jumpPlayback(double positionSeconds) = 0;
        virtual void startPlayback() = 0;
        virtual void stopPlayback() = 0;
        virtual void pausePlayback() = 0;
        virtual void resumePlayback() = 0;
        // Audio analysis. The analyser nodes can also be reused directly in an
        // AudioPluginGraph. Consumers select time- or frequency-domain data from
        // the node instead of using engine-specific analysis adapters.
        virtual uapmd_graph::webaudio_compat::AnalyserNode* inputAnalyser() = 0;
        virtual uapmd_graph::webaudio_compat::AnalyserNode* outputAnalyser() = 0;

        // Convenience methods for sending MIDI events
        virtual void sendNoteOn(int32_t instanceId, int32_t note) = 0;
        virtual void sendNoteOff(int32_t instanceId, int32_t note) = 0;
        virtual void sendPitchBend(int32_t instanceId, float normalizedValue) = 0;
        virtual void sendChannelPressure(int32_t instanceId, float pressure) = 0;
        virtual void setParameterValue(int32_t instanceId, int32_t index, double value) = 0;

        // Timeline clip management and project loading
        virtual TimelineFacade& timeline() = 0;

        static std::unique_ptr<SequencerEngine> create(
            int32_t sampleRate,
            size_t audioBufferSizeInFrames,
            size_t umpBufferSizeInInts);
    };

}
