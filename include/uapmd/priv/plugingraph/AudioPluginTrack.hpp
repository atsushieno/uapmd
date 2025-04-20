#pragma once
#include "uapmd/priv/CommonTypes.hpp"
#include "AudioPluginGraph.hpp"

namespace uapmd {

    class AudioPluginTrack {
        class Impl;
        Impl* impl;

    public:
        explicit AudioPluginTrack(size_t eventBufferSizeInBytes);
        ~AudioPluginTrack();

        AudioPluginGraph& graph();

        bool bypassed();
        bool frozen();
        void bypassed(bool value);
        void frozen(bool value);

        bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size);

        int32_t processAudio(AudioProcessContext& process);
    };

}
