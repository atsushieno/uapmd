#include "remidy-tooling/PluginInstancing.hpp"

#include <cassert>
#include <iostream>
#include <format>

void remidy_tooling::PluginInstancing::setupInstance(remidy::PluginUIThreadRequirement uiThreadRequirement, std::function<void(std::string error)> callback) {
    Logger::global()->logInfo("  instantiating %s %s", format->name().c_str(), displayName.c_str());
    instancing_state = PluginInstancingState::Preparing;

    auto cb = [this,callback](std::unique_ptr<PluginInstance> newInstance, std::string error) {
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
    };
    format->createInstance(entry, remidy::PluginFormat::PluginInstantiationOptions{uiThreadRequirement}, cb);
}

remidy::PluginFormat* findFormat(remidy_tooling::PluginScanTool& scanner, const std::string_view& format) {
    for (auto f : scanner.formats())
        if (f->name() == format)
            return f;
    return nullptr;
}
remidy::PluginCatalogEntry* findPlugin(remidy_tooling::PluginScanTool& scanner, const std::string_view& format, const std::string_view& pluginId) {
    for (auto e : scanner.catalog.getPlugins())
        if (e->format() == format && e->pluginId() == pluginId)
            return e;
    return nullptr;
}

remidy_tooling::PluginInstancing::PluginInstancing(remidy_tooling::PluginScanTool &scanner,
                                                   const std::string_view &format, const std::string_view &pluginId) :
    scanner(scanner), format(findFormat(scanner, format)), entry(findPlugin(scanner, format, pluginId)) {
        displayName = entry->displayName();
}

remidy_tooling::PluginInstancing::PluginInstancing(PluginScanTool& scanner, PluginFormat* format, PluginCatalogEntry* entry) :
    scanner(scanner), format(format), entry(entry) {
    displayName = entry->displayName();
}


remidy_tooling::PluginInstancing::~PluginInstancing() {
    // Allow destruction during async instantiation (e.g., if instantiation fails or is cancelled)
    if (instancing_state == PluginInstancingState::Preparing) {
        Logger::global()->logWarning("  %s: %s destroyed while still preparing (async instantiation likely failed or cancelled)",
                                    format->name().c_str(), displayName.c_str());
        instancing_state = PluginInstancingState::Error;
    }
    if (!instance)
        return;
    if (instancing_state == PluginInstancingState::Ready) {
        instancing_state = PluginInstancingState::Terminating;
        auto code = instance->stopProcessing();
        if (code != StatusCode::OK)
            std::cerr << "  " << format->name() << ": " << displayName << " : stopProcessing() failed. Error code " << (int32_t) code << std::endl;
    }
    auto uiReq = format->requiresUIThreadOn(instance->info());
    if (uiReq & PluginUIThreadRequirement::InstanceControl) {
        if (EventLoop::runningOnMainThread()) {
            instance.reset();
        } else {
            // At shutdown, the UI loop may be gone; tear down synchronously to avoid dangling threads
            instance.reset();
        }
    } else {
        instance.reset();
    }
    instancing_state = PluginInstancingState::Terminated;
}

void remidy_tooling::PluginInstancing::makeAlive(std::function<void(std::string error)> callback) {
    if (scanner.shouldCreateInstanceOnUIThread(format, entry)) {
        EventLoop::runTaskOnMainThread([this,callback] {
            setupInstance(remidy::PluginUIThreadRequirement::AllNonAudioOperation, callback);
        });
    }
    else
        setupInstance(remidy::PluginUIThreadRequirement::None, callback);
}
