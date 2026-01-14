#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "uapmd/uapmd.hpp"
#include "AudioPluginGraph.hpp"

namespace uapmd {

    class AudioPluginTrack {
    protected:
        AudioPluginTrack() = default;

    public:
        virtual ~AudioPluginTrack() = default;
        static std::unique_ptr<AudioPluginTrack> create(size_t eventBufferSizeInBytes);

        virtual AudioPluginGraph& graph() = 0;

        virtual bool bypassed() = 0;
        virtual bool frozen() = 0;
        virtual void bypassed(bool value) = 0;
        virtual void frozen(bool value) = 0;

        virtual bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) = 0;

        virtual int32_t processAudio(AudioProcessContext& process) = 0;

        virtual void setGroupResolver(std::function<uint8_t(int32_t)> resolver) = 0;
        virtual void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) = 0;
    };

}
