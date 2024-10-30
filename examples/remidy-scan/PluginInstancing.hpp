#pragma once
#include <cassert>
#include <cpplocate/cpplocate.h>
#include <remidy/remidy.hpp>
#include "PluginScanner.hpp"

namespace remidy {
    enum class PluginInstancingState {
        Created,
        Preparing,
        Ready,
        Error,
        Terminating,
        Terminated
    };

    class PluginInstancing {
        PluginScanner& scanner;
        AudioPluginFormat* format{};
        PluginCatalogEntry* entry{};
        AudioPluginInstance::ConfigurationRequest config{};
        std::unique_ptr<AudioPluginInstance> instance{nullptr};
        std::string displayName;
        std::atomic<PluginInstancingState> instancing_state{PluginInstancingState::Created};

        void setupInstance(std::function<void(std::string error)>&& callback);

    public:
        explicit PluginInstancing(PluginScanner& scanner, AudioPluginFormat* format, PluginCatalogEntry* entry);
        ~PluginInstancing();
        void makeAlive(std::function<void(std::string error)>&& callback);

        void withInstance(std::function<void(AudioPluginInstance*)>&& callback) const {
            assert(instancing_state != PluginInstancingState::Preparing);
            if (instancing_state != PluginInstancingState::Ready || !instance)
                return;
            callback(instance.get());
        }

        std::atomic<PluginInstancingState>& instancingState() { return instancing_state; }
    };
}