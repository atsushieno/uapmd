#pragma once
#include <functional>
#include <vector>

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

        void setGroupResolver(std::function<uint8_t(int32_t)> resolver);
        void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback);
    };

}
