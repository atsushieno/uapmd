#pragma once

#include "../plugin-api/AudioPluginInstanceAPI.hpp"

namespace uapmd {

    class SequencerFeature {
    public:
        virtual ~SequencerFeature() = default;

        // Event routing
        virtual void enqueueUmp(int32_t instanceId, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) = 0;

        using PluginOutputHandler = std::function<void(const uapmd_ump_t*, size_t)>;
        virtual void setPluginOutputHandler(int32_t instanceId, PluginOutputHandler handler) = 0;

        virtual AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) = 0;
    };

}
