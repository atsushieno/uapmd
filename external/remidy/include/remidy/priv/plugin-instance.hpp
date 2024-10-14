#pragma once

#include "../remidy.hpp"

namespace remidy {

    class AudioPluginInstance {
    protected:
        explicit AudioPluginInstance() = default;

    public:
        virtual ~AudioPluginInstance() = default;

        virtual AudioPluginExtensibility<AudioPluginInstance>* getExtensibility() { return nullptr; }

        virtual StatusCode configure(int32_t sampleRate, bool offlineMode) = 0;

        virtual StatusCode startProcessing() = 0;

        virtual StatusCode stopProcessing() = 0;

        virtual StatusCode process(AudioProcessContext& process) = 0;
    };

}