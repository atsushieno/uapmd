#pragma once
#include <cstdint>

#include "../AudioPluginTrack.hpp"

namespace uapmd {

    class AudioPluginHostImpl {
    public:
        AudioPluginTrack* getTrack(int32_t index);
    };

}
