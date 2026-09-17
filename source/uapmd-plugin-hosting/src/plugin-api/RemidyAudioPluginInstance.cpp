#include "RemidyAudioPluginInstance.hpp"

namespace uapmd_plugin_hosting {

    void notifyTimingInfoChangeIfNeeded(
        remidy::PluginInstance& instance,
        uint32_t previousLatency,
        double previousTail) {
        const auto currentLatency = instance.latencyInSamples();
        const auto currentTail = instance.tailLengthInSeconds();
        if (currentLatency == previousLatency && currentTail == previousTail)
            return;
        instance.timingInfoChangeEvent().notify({
            .latency_changed = currentLatency != previousLatency,
            .tail_changed = currentTail != previousTail,
        });
    }

    remidy::PluginStateSupport::StateContextType toRemidyStateContextType(StateContextType type) {
        switch (type) {
            case StateContextType::Remember:
                return remidy::PluginStateSupport::StateContextType::Remember;
            case StateContextType::Copyable:
                return remidy::PluginStateSupport::StateContextType::Copyable;
            case StateContextType::Preset:
                return remidy::PluginStateSupport::StateContextType::Preset;
            case StateContextType::Project:
                return remidy::PluginStateSupport::StateContextType::Project;
        }
        return remidy::PluginStateSupport::StateContextType::Project;
    }

}
