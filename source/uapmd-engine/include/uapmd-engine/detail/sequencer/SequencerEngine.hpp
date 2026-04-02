#pragma once
#include <limits>
#include <memory>
#include <vector>

#include <uapmd/uapmd.hpp>
#include "TimelineFacade.hpp"

namespace uapmd {
    inline constexpr int32_t kMasterTrackIndex = std::numeric_limits<int32_t>::min();
    typedef int32_t uapmd_track_index_t;

    class AudioPluginHostingAPI;

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

        virtual AudioPluginHostingAPI* pluginHost() = 0;
        virtual AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) = 0;

        virtual UapmdFunctionBlockManager* functionBlockManager() = 0;
        // FIXME: we should probably remove this at some stage
        virtual int32_t findTrackIndexForInstance(int32_t instanceId) const = 0;

        virtual std::vector<SequencerTrack *> & tracks() const = 0;
        virtual SequencerTrack* masterTrack() = 0;
        virtual uint32_t trackLatencyInSamples(uapmd_track_index_t trackIndex) = 0;
        virtual uint32_t masterTrackLatencyInSamples() = 0;
        virtual uint32_t trackRenderLeadInSamples(uapmd_track_index_t trackIndex) = 0;
        virtual uint32_t masterTrackRenderLeadInSamples() = 0;
        virtual bool trackHasLiveInput(uapmd_track_index_t trackIndex) = 0;
        virtual uint32_t trackOutputAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex) = 0;
        virtual uint32_t trackOutputBusAlignmentHoldbackInSamples(uapmd_track_index_t trackIndex, uint32_t outputBusIndex) = 0;
        virtual bool isOutputAlignmentActive() = 0;
        // Create track with plugin + configure bus (replaces manual addSimpleTrack + configureMainBus pattern)
        virtual uapmd_track_index_t addEmptyTrack() = 0;
        // Add plugin to existing track
        virtual void addPluginToTrack(uapmd_track_index_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, uapmd_track_index_t trackIndex, std::string error)> callback) = 0;
        virtual bool removePluginInstance(int32_t instanceId) = 0;
        virtual bool removeTrack(uapmd_track_index_t trackIndex) = 0;

        // UMP group assignment helpers — search all tracks for the given instanceId.
        // getInstanceGroup returns 0xFF if the instance is not found.
        // setInstanceGroup returns false if the requested group is already taken on that track.
        virtual uint8_t getInstanceGroup(int32_t instanceId) const = 0;
        virtual bool    setInstanceGroup(int32_t instanceId, uint8_t group) = 0;
        // Clean up empty tracks (must be called from non-audio thread)
        virtual void cleanupEmptyTracks() = 0;

        // Set default channel configuration (called by RealtimeSequencer when device changes)
        virtual void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) = 0;
        virtual bool offlineRendering() const = 0;
        virtual void offlineRendering(bool enabled) = 0;

        virtual void setEngineActive(bool active) = 0;

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

        // When enabled, processAudio() skips the inline pumpAudio() call.
        // Set to true by WebAudioEngineThread before starting its pump pthread so
        // that the pump thread and the engine thread do not both call pumpAudio()
        // concurrently.  On all non-Emscripten platforms this remains false and
        // processAudio() continues to call pumpAudio() inline as before.
        virtual void setExternalPump(bool enabled) = 0;
        using TrackOutputHandler = std::function<bool(uapmd_track_index_t, SequencerTrack&, AudioProcessContext&)>;
        virtual void setTrackOutputHandler(TrackOutputHandler handler) = 0;

        // RT plugin chain: calls AudioPluginGraph::processAudio() for every track, mixes
        // outputs, and runs the master track. In single-threaded builds this is called
        // after pumpAudio(); in the Emscripten build it will be called from
        // WebAudioEngineThread independently.
        virtual uapmd_status_t processAudio(AudioProcessContext& process) = 0;

        // Playback control (accessed by RealtimeSequencer)
        virtual bool isPlaybackActive() const = 0;
        virtual void playbackPosition(int64_t samples) = 0;
        virtual int64_t playbackPosition() const = 0;
        virtual int64_t renderPlaybackPosition() const = 0;
        virtual void startPlayback() = 0;
        virtual void stopPlayback() = 0;
        virtual void pausePlayback() = 0;
        virtual void resumePlayback() = 0;

        // Audio analysis
        // FIXME: they should be replaced by direct access to current audio buffers.
        virtual void getInputSpectrum(float* outSpectrum, int numBars) const = 0;
        virtual void getOutputSpectrum(float* outSpectrum, int numBars) const = 0;

        // Convenience methods for sending MIDI events
        virtual void sendNoteOn(int32_t instanceId, int32_t note) = 0;
        virtual void sendNoteOff(int32_t instanceId, int32_t note) = 0;
        virtual void sendPitchBend(int32_t instanceId, float normalizedValue) = 0;
        virtual void sendChannelPressure(int32_t instanceId, float pressure) = 0;
        virtual void setParameterValue(int32_t instanceId, int32_t index, double value) = 0;

        // Timeline clip management and project loading
        virtual TimelineFacade& timeline() = 0;

        static std::unique_ptr<SequencerEngine> create(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts);
    };

}
