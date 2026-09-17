#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

#include <iostream>
#include <format>

void uapmd_plugin_hosting::PluginInstancing::setupInstance(AudioPluginUIThreadRequirement uiThreadRequirement, std::function<void(std::string error)> callback) {
    Logger::global()->logInfo("  instantiating %s %s", format->name().c_str(), displayName.c_str());
    instancing_state = PluginInstancingState::Preparing;

    auto options = config;
    options.uiThreadRequirement = uiThreadRequirement;

    auto cb = [this,callback](std::unique_ptr<AudioPluginInstanceAPI> newInstance, std::string error) {
        if (!error.empty()) {
            instancing_state = PluginInstancingState::Error;
            callback(std::format("  {}: {} : {}", format->name(), displayName, error));
            return;
        }
        setCurrentThreadNameIfPossible(std::format("uapmd-scan.{}:{}", format->name(), entry->displayName()));
        instance_ = std::move(newInstance);
        if (!instance_)
            error = std::format("  {}: Could not instantiate plugin {}.", format->name(), displayName);
        else {
            auto code = instance_->startProcessing();
            if (code != 0)
                error = std::format("  {}: {} : startProcessing() failed. Error code {}", format->name(), displayName, code);
            else {
                instancing_state = PluginInstancingState::Ready;
                callback("");
                return;
            }
        }
        callback(error);
        instancing_state = PluginInstancingState::Error;
    };
    format->createInstance(entry, options, cb);
}

uapmd_plugin_hosting::AudioPluginFormat* findFormat(uapmd_plugin_hosting::PluginScanTool& scanner, const std::string_view& format) {
    for (auto f : scanner.formats())
        if (f->name() == format)
            return f;
    return nullptr;
}
uapmd_plugin_hosting::AudioPluginCatalogEntry* findPlugin(uapmd_plugin_hosting::PluginScanTool& scanner, const std::string_view& format, const std::string_view& pluginId) {
    for (auto e : scanner.catalog().getPlugins())
        if (e->format() == format && e->pluginId() == pluginId)
            return e;
    return nullptr;
}

uapmd_plugin_hosting::PluginInstancing::PluginInstancing(uapmd_plugin_hosting::PluginScanTool &scanner,
                                                   const std::string_view &format, const std::string_view &pluginId) :
    scanner(scanner), format(findFormat(scanner, format)), entry(findPlugin(scanner, format, pluginId)) {
        displayName = entry->displayName();
}

uapmd_plugin_hosting::PluginInstancing::PluginInstancing(PluginScanTool& scanner, AudioPluginFormat* format, AudioPluginCatalogEntry* entry) :
    scanner(scanner), format(format), entry(entry) {
    displayName = entry->displayName();
}


uapmd_plugin_hosting::PluginInstancing::~PluginInstancing() {
    // Allow destruction during async instantiation (e.g., if instantiation fails or is cancelled)
    if (instancing_state == PluginInstancingState::Preparing) {
        Logger::global()->logWarning("  %s: %s destroyed while still preparing (async instantiation likely failed or cancelled)",
                                    format->name().c_str(), displayName.c_str());
        instancing_state = PluginInstancingState::Error;
    }
    if (!instance_)
        return;
    // Tear the editor down before processing stops: a plugin editor may still talk to its
    // processor, and this is the order the plugin formats expect.
    instance_->destroyUI();
    if (instancing_state == PluginInstancingState::Ready) {
        instancing_state = PluginInstancingState::Terminating;
        auto code = instance_->stopProcessing();
        if (code != 0)
            std::cerr << "  " << format->name() << ": " << displayName << " : stopProcessing() failed. Error code " << code << std::endl;
    }
    instance_.reset();
    instancing_state = PluginInstancingState::Terminated;
}

void uapmd_plugin_hosting::PluginInstancing::makeAlive(std::function<void(std::string error)> callback) {
    if (scanner.shouldCreateInstanceOnUIThread(format, entry)) {
        EventLoop::runTaskOnMainThread([this,callback] {
            setupInstance(UIThreadForAllNonAudioOperation, callback);
        });
    }
    else
        setupInstance(UIThreadNotRequired, callback);
}
