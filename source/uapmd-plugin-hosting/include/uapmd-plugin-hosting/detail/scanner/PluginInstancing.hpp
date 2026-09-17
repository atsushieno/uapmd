#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>

#include "../plugin-api/AudioPluginFormat.hpp"
#include "../plugin-api/AudioPluginInstanceAPI.hpp"
#include "PluginScanTool.hpp"

namespace uapmd_plugin_hosting {

    enum class PluginInstancingState {
        Created,
        Preparing,
        Ready,
        Error,
        Terminating,
        Terminated
    };

    // Drives one plugin instance through its lifecycle: instantiate it on whichever thread
    // the format demands, configure it, start processing, and tear it down in order.
    class PluginInstancing {
        PluginScanTool& scanner;
        AudioPluginFormat* format{};
        AudioPluginCatalogEntry* entry{};
        AudioPluginInstantiationOptions config{};
        std::unique_ptr<AudioPluginInstanceAPI> instance_{nullptr};
        std::string displayName;
        std::atomic<PluginInstancingState> instancing_state{PluginInstancingState::Created};

        void setupInstance(AudioPluginUIThreadRequirement uiThreadRequirement, std::function<void(std::string error)> callback);

    public:
        explicit PluginInstancing(PluginScanTool& scanner, const std::string_view& format, const std::string_view& pluginId);
        explicit PluginInstancing(PluginScanTool& scanner, AudioPluginFormat* format, AudioPluginCatalogEntry* entry);
        ~PluginInstancing();
        void makeAlive(std::function<void(std::string error)> callback);

        void withInstance(std::function<void(AudioPluginInstanceAPI*)> callback) const {
            assert(instancing_state != PluginInstancingState::Preparing);
            if (instancing_state != PluginInstancingState::Ready || !instance_)
                return;
            callback(instance_.get());
        }

        // The instance, once it is ready, and null until then. It stays owned here: this
        // object is what stops processing and tears it down in the right order.
        AudioPluginInstanceAPI* instance() const {
            return instancing_state == PluginInstancingState::Ready ? instance_.get() : nullptr;
        }

        // The configuration the instance is created with. It has to be set before
        // makeAlive(), because the format configures the plugin while instantiating it.
        AudioPluginInstantiationOptions& configurationRequest() { return config; }

        std::atomic<PluginInstancingState>& instancingState() { return instancing_state; }
    };
}
