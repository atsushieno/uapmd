#pragma once
#include <memory>

#include "uapmd/uapmd.hpp"

namespace uapmd {
    class AudioPluginHostingAPI;

    // Parameter update from plugin (via NRPN or direct listener)
    struct ParameterUpdate {
        int32_t parameterIndex;
        double value;
    };

    // A sequence processor that works as a facade for the overall audio processing at each AudioPluginTrack.
    // It is used to enqueue input events to each audio track, to process once at a time when an audio I/O event arrives.
    // It is independent of DeviceIODispatcher (it will fire `processAudio()`)..
    // It is also independent of the timed sequencer that is to deliver sequencer inputs on time.
    class SequencerEngine {
    protected:
        SequencerEngine() = default;

    public:
        struct PluginNodeInfo {
            int32_t instanceId = -1;
            std::string pluginId;
            std::string format;
            std::string displayName;
        };

        struct TrackInfo {
            int32_t trackIndex = -1;
            std::vector<PluginNodeInfo> nodes;
        };

        virtual ~SequencerEngine() = default;
        static std::unique_ptr<SequencerEngine> create(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts);

        virtual void performPluginScanning(bool rescan) = 0;

        virtual PluginCatalog& catalog() = 0;
        virtual std::string getPluginName(int32_t instanceId) = 0;

        virtual SequenceProcessContext& data() = 0;

        virtual std::vector<AudioPluginTrack *> & tracks() const = 0;

        virtual std::vector<TrackInfo> getTrackInfos() = 0;

        // Set default channel configuration (called by AudioPluginSequencer when device changes)
        virtual void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) = 0;

        // Create track with plugin + configure bus (replaces manual addSimpleTrack + configureMainBus pattern)
        virtual void addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackIndex, std::string error)> callback) = 0;

        // Add plugin to existing track
        virtual void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(int32_t instanceId, int32_t trackId, std::string error)> callback) = 0;

        virtual bool removePluginInstance(int32_t instanceId) = 0;

        virtual uapmd_status_t processAudio(AudioProcessContext& process) = 0;

        // Playback control (accessed by AudioPluginSequencer)
        virtual bool isPlaybackActive() const = 0;
        virtual void setPlaybackPosition(int64_t samples) = 0;
        virtual int64_t playbackPosition() const = 0;
        virtual void startPlayback() = 0;
        virtual void stopPlayback() = 0;
        virtual void pausePlayback() = 0;
        virtual void resumePlayback() = 0;

        // Audio file playback
        virtual void loadAudioFile(std::unique_ptr<AudioFileReader> reader) = 0;
        virtual void unloadAudioFile() = 0;
        virtual double audioFileDurationSeconds() const = 0;

        // Audio analysis
        virtual void getInputSpectrum(float* outSpectrum, int numBars) const = 0;
        virtual void getOutputSpectrum(float* outSpectrum, int numBars) const = 0;

        // Plugin instance queries
        virtual AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) = 0;
        virtual bool isPluginBypassed(int32_t instanceId) = 0;
        virtual void setPluginBypassed(int32_t instanceId, bool bypassed) = 0;

        // Group queries
        virtual std::optional<uint8_t> groupForInstance(int32_t instanceId) const = 0;
        virtual std::optional<int32_t> instanceForGroup(uint8_t group) const = 0;

        // Event routing
        virtual void enqueueUmpForInstance(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) = 0;
        virtual void enqueueUmp(int32_t targetId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) = 0;

        // Convenience methods for sending MIDI events
        virtual void sendNoteOn(int32_t targetId, int32_t note) = 0;
        virtual void sendNoteOff(int32_t targetId, int32_t note) = 0;
        virtual void setParameterValue(int32_t instanceId, int32_t index, double value) = 0;

        // Parameter listening
        virtual void registerParameterListener(int32_t instanceId, AudioPluginInstanceAPI* instance) = 0;
        virtual void unregisterParameterListener(int32_t instanceId) = 0;
        virtual std::vector<ParameterUpdate> getParameterUpdates(int32_t instanceId) = 0;
    };

}
