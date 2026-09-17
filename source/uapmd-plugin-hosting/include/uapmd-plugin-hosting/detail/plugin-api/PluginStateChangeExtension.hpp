#pragma once

#include <functional>
#include <string_view>
#include "AudioPluginInstanceAPI.hpp"

// Lets a plugin report that it changed its own internal state, without the host having
// asked it to. The host records that as the document becoming dirty.
//
// Most formats never register this: the host is the single source of truth for their
// state, so there is nothing for the plugin to report. Among the formats we ship, only
// VST3 does. A format whose plugins can be edited in their own editor (a script, a
// sample mapping) will want to register it too.
namespace uapmd_plugin_hosting {
    inline constexpr std::string_view kPluginStateChangeExtensionId =
        "dev.atsushieno.uapmd.plugin-instance.state-change.v1";

    class PluginStateChangeExtension : public AudioPluginInstanceExtension {
    public:
        std::string_view extensionId() const override {
            return kPluginStateChangeExtensionId;
        }

        // Sets the handler the instance calls when it changed its own state. There is one
        // handler, set by whoever owns the instance, and it lives as long as the instance.
        virtual void onPluginStateChanged(std::function<void()> handler) = 0;
    };
}
