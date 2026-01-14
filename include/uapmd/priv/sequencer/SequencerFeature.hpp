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

        // Group queries
        virtual std::optional<uint8_t> groupForInstance(int32_t instanceId) const = 0;
        virtual std::optional<int32_t> instanceForGroup(uint8_t group) const = 0;

        virtual AudioPluginInstanceAPI* getPluginInstance(int32_t instanceId) = 0;
    };

}