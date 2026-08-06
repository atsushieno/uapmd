#pragma once

#include <cstdint>

#include "../CommonTypes.hpp"
#include "AudioPluginInstanceAPI.hpp"

namespace uapmd {

    using EventListenerId = int64_t;

    class AudioPluginNodeFeature {
    public:
        virtual ~AudioPluginNodeFeature() = default;

        virtual uapmd_plugin_hosting::AudioPluginInstanceAPI* instance() = 0;
        virtual bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) = 0;
    };

}
