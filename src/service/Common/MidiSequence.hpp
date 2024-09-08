#pragma once
#include <vector>

#include "CommonTypes.hpp"

// Represents a sample-accurate sequence of UMPs
class MidiSequence {
    std::vector<uapmd_ump_t> messages;
public:
    uapmd_ump_t* getMessages();
    size_t sizeInInts();
    size_t sizeInBytes();
};
