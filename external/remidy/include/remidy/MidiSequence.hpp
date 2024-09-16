#pragma once
#include <vector>

#include "Common.hpp"

namespace remidy {

    // Represents a sample-accurate sequence of UMPs
    class MidiSequence {
        std::vector<remidy_ump_t> messages;
    public:
        remidy_ump_t* getMessages();
        size_t sizeInInts();
        size_t sizeInBytes();
    };

}
