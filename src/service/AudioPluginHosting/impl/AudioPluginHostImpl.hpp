#pragma once
#include <cstdint>

#include "../AudioPluginTrack.hpp"

class AudioPluginHostImpl {
public:
    AudioPluginTrack* getTrack(int32_t index);
};
