#pragma once

#include "../remidy.hpp"

namespace remidy {

    enum class AudioContentType {
        Float32,
        Float64
    };

    class AudioPluginInstance {
    protected:
        explicit AudioPluginInstance() = default;

    public:
        struct Configuration {
            int32_t sampleRate = 48000;
            bool offlineMode = false;
            AudioContentType dataType = AudioContentType::Float32;
        };

        virtual ~AudioPluginInstance() = default;

        virtual AudioPluginExtensibility<AudioPluginInstance>* getExtensibility() { return nullptr; }

        virtual StatusCode configure(Configuration& configuration) = 0;

        virtual StatusCode startProcessing() = 0;

        virtual StatusCode stopProcessing() = 0;

        virtual StatusCode process(AudioProcessContext& process) = 0;
    };

}