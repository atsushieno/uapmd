#pragma once
#include <memory>

#include "AudioPluginTrack.hpp"

class AudioPluginHost {
public:
    static std::unique_ptr<AudioPluginHost> create();

    AudioPluginTrack* getTrack(int32_t index);
};
