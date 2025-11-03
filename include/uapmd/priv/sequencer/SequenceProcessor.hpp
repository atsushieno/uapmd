#pragma once
#include <memory>

#include "uapmd/uapmd.hpp"

namespace uapmd {
    class AudioPluginHostPAL;

    // A sequence processor that works as a facade for the overall audio processing at each AudioPluginTrack.
    // It is used to enqueue input events to each audio track, to process once at a time when an audio I/O event arrives.
    // It is independent of DeviceIODispatcher (it will fire `processAudio()`)..
    // It is also independent of the timed sequencer that is to deliver sequencer inputs on time.
    class SequenceProcessor {
        class Impl;
        Impl *impl;

    public:
        explicit SequenceProcessor(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostPAL* pal = AudioPluginHostPAL::instance());
        ~SequenceProcessor();

        SequenceProcessContext& data();

        std::vector<AudioPluginTrack *> & tracks() const;

        void addSimpleTrack(std::string& format, std::string& pluginId, uint32_t inputChannels, uint32_t outputChannels, std::function<void(AudioPluginTrack* track, std::string error)> callback);
        bool removePluginInstance(int32_t instanceId);

        uapmd_status_t processAudio();
    };

}
