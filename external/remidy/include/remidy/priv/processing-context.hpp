#pragma once

#include "../remidy.hpp"

namespace remidy {

    // Represents a list of audio buffers, separate per channel.
    // It is part of `AudioProcessingContext`.
    class AudioBufferList {
    public:
        float* getFloatBufferForChannel(int32_t channel);
        double* getDoubleBufferForChannel(int32_t channel);
        int32_t size();
    };

    // Represents a sample-accurate sequence of UMPs.
    // It is part of `AudioProcessingContext`.
    class MidiSequence {
        std::vector<remidy_ump_t> messages;
    public:
        remidy_ump_t* getMessages();
        size_t sizeInInts();
        size_t sizeInBytes();
    };

    class AudioProcessContext {
    public:
        AudioProcessContext();

        AudioBufferList input{};
        AudioBufferList output{};
        MidiSequence midi{};
    };

}