#pragma once

#include <cstdint>

namespace uapmd_plugin_hosting {
class AudioPluginInstanceAPI;
}

namespace uapmd {

// Non-realtime notification for services that own state associated with hosted
// plugin instances. Listeners are invoked on the control thread.
class PluginInstanceLifecycleListener {
public:
    virtual ~PluginInstanceLifecycleListener() = default;

    virtual void pluginInstanceAdded(
        int32_t,
        uapmd_plugin_hosting::AudioPluginInstanceAPI&) {}
    virtual void pluginInstanceWillBeDestroyed(int32_t) {}
};

} // namespace uapmd
