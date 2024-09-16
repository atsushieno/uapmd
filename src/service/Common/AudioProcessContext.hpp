
#pragma once

#include "AudioBufferList.hpp"
#include "MidiSequence.hpp"

namespace uapmd {
    class AudioProcessContext {
    public:
        AudioProcessContext();

        AudioBufferList input{};
        AudioBufferList output{};
        MidiSequence midi{};
    };
}
