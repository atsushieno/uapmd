#pragma once

#include <cassert>
#include "remidy/remidy.hpp"
#include "PluginScanning.hpp"

namespace remidy_tooling {
    using namespace remidy;

    enum class PluginInstancingState {
        Created,
        Preparing,
        Ready,
        Error,
        Terminating,
        Terminated
    };

    class PluginInstancing {
        PluginScanning& scanner;
        PluginFormat* format{};
        PluginCatalogEntry* entry{};
        PluginInstance::ConfigurationRequest config{};
        std::unique_ptr<PluginInstance> instance{nullptr};
        std::string displayName;
        std::atomic<PluginInstancingState> instancing_state{PluginInstancingState::Created};

        void setupInstance(std::function<void(std::string error)> callback);

    public:
        explicit PluginInstancing(PluginScanning& scanner, PluginFormat* format, PluginCatalogEntry* entry);
        ~PluginInstancing();
        void makeAlive(std::function<void(std::string error)> callback);

        void withInstance(std::function<void(PluginInstance*)> callback) const {
            assert(instancing_state != PluginInstancingState::Preparing);
            if (instancing_state != PluginInstancingState::Ready || !instance)
                return;
            callback(instance.get());
        }

        std::atomic<PluginInstancingState>& instancingState() { return instancing_state; }
    };
}