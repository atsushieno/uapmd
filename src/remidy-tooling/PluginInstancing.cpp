#include "remidy-tooling/PluginInstancing.hpp"

#include <cassert>
#include <iostream>

void remidy_tooling::PluginInstancing::setupInstance(std::function<void(std::string error)>&& callback) {
    std::cerr << "  instantiating " << format->name() << " " << displayName << std::endl;
    instancing_state = PluginInstancingState::Preparing;
    auto cb = std::move(callback);

    format->createInstance(entry, [&](std::unique_ptr<PluginInstance> newInstance, std::string error) {
        auto callback = std::move(cb);
        if (!error.empty()) {
            instancing_state = PluginInstancingState::Error;
            callback(error);
            return;
        }
        setCurrentThreadNameIfPossible(std::format("remidy-scan.{}:{}", format->name(), entry->displayName()));
        instance = std::move(newInstance);
        if (!instance)
            error = std::format("  {}: Could not instantiate plugin {}. Details: {}", format->name(), displayName, error);
        else {
            auto code = instance->configure(config);
            if (code != StatusCode::OK)
                error = std::format("  {}: {} : configure() failed. Error code {}", format->name(), displayName, (int32_t) code);
            else {
                auto code = instance->startProcessing();
                if (code != StatusCode::OK)
                    error = std::format("  {}: {} : startProcessing() failed. Error code {}", format->name(), displayName, (int32_t) code);
                else {
                    instancing_state = PluginInstancingState::Ready;
                    callback("");
                    return;
                }
            }
        }
        callback(error);
        instancing_state = PluginInstancingState::Error;
    });
}

remidy_tooling::PluginInstancing::PluginInstancing(PluginScanning& scanner, PluginFormat* format, PluginCatalogEntry* entry) :
    scanner(scanner), format(format), entry(entry) {
    displayName = entry->displayName();
}


remidy_tooling::PluginInstancing::~PluginInstancing() {
    assert(instancing_state != PluginInstancingState::Preparing);
    if (!instance)
        return;
    if (instancing_state == PluginInstancingState::Ready) {
        instancing_state = PluginInstancingState::Terminating;
        auto code = instance->stopProcessing();
        if (code != StatusCode::OK)
            std::cerr << "  " << format->name() << ": " << displayName << " : stopProcessing() failed. Error code " << (int32_t) code << std::endl;
    }
    instancing_state = PluginInstancingState::Terminated;
}

void remidy_tooling::PluginInstancing::makeAlive(std::function<void(std::string error)>&& callback) {
    if (scanner.shouldCreateInstanceOnUIThread(format, entry)) {
        EventLoop::runTaskOnMainThread([&] {
            setupInstance(std::move(callback));
        });
    }
    else
        setupInstance(std::move(callback));
}
