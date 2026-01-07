#pragma once
#include <memory>

#include "uapmd/uapmd.hpp"

namespace uapmd {
    class AudioPluginHostingAPI;

    // A sequence processor that works as a facade for the overall audio processing at each AudioPluginTrack.
    // It is used to enqueue input events to each audio track, to process once at a time when an audio I/O event arrives.
    // It is independent of DeviceIODispatcher (it will fire `processAudio()`)..
    // It is also independent of the timed sequencer that is to deliver sequencer inputs on time.
    class SequenceProcessor {
    protected:
        SequenceProcessor() = default;

    public:
        virtual ~SequenceProcessor() = default;
        static std::unique_ptr<SequenceProcessor> create(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal = AudioPluginHostingAPI::instance());

        virtual SequenceProcessContext& data() = 0;

        virtual std::vector<AudioPluginTrack *> & tracks() const = 0;

        // Set default channel configuration (called by AudioPluginSequencer when device changes)
        virtual void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) = 0;

        // Create track with plugin + configure bus (replaces manual addSimpleTrack + configureMainBus pattern)
        virtual void addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, AudioPluginTrack* track, int32_t trackIndex, std::string error)> callback) = 0;

        // Add plugin to existing track
        virtual void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, std::string error)> callback) = 0;

        virtual bool removePluginInstance(int32_t instanceId) = 0;

        virtual uapmd_status_t processAudio() = 0;
    };

}
