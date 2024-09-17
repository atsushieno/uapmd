#pragma once
#include <cstdint>

#include "Common.hpp"

namespace remidy {
    class AudioProcessContext;

    class AudioPluginInstance {
    protected:
        explicit AudioPluginInstance() = default;

    public:
        virtual ~AudioPluginInstance() = default;

        class Extensibility {
            AudioPluginInstance& owner;
        protected:
            explicit Extensibility(AudioPluginInstance& owner) : owner(owner) {
            }
            virtual ~Extensibility() = default;
        };

        virtual Extensibility* getExtensibility() { return nullptr; }

        virtual remidy_status_t configure(int32_t sampleRate) = 0;

        virtual remidy_status_t process(AudioProcessContext& process) = 0;
    };
}
