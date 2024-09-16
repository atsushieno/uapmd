#pragma once
#include <cstdint>

#include "Common.hpp"

namespace remidy {
    class AudioProcessContext;

    class AudioPluginInstance {
    protected:
        virtual ~AudioPluginInstance() = default;
    public:

        virtual remidy_status_t configure(int32_t sampleRate) = 0;

        virtual remidy_status_t process(AudioProcessContext& process) = 0;
    };
}
