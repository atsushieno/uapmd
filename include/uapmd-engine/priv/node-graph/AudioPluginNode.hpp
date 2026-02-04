#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "uapmd/uapmd.hpp"

namespace uapmd {

    // AudioPluginNode wraps a plugin instance with its own event queue.
    // This allows per-instance event routing instead of per-track routing.
    // Managed internally by AudioPluginGraph.
    class AudioPluginNode {
    public:
        virtual ~AudioPluginNode() = default;

        virtual int32_t instanceId() const = 0;
        virtual AudioPluginInstanceAPI* instance() = 0;

        // Schedule UMP events to this plugin instance's queue
        virtual bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) = 0;
    };

}
