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

        virtual int32_t processAudio(AudioProcessContext& process) = 0;

        // For safe deletion: check if audio thread is currently processing this track
        virtual bool isProcessing() const = 0;
    };

}
