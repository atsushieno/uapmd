
#pragma once

#include "AudioBufferList.hpp"
#include "MidiSequence.hpp"

namespace remidy {
    class AudioProcessContext {
    public:
        AudioProcessContext();

        AudioBufferList input{};
        AudioBufferList output{};
        MidiSequence midi{};
    };
}
