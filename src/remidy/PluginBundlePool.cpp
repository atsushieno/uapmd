
#include "remidy.hpp"

#include <iostream>

remidy::PluginBundlePool::PluginBundlePool(
    std::function<StatusCode(std::filesystem::path& moduleBundlePath, void** module)>& load,
    std::function<StatusCode(std::filesystem::path& moduleBundlePath, void* module)>& unload
) : load(load), unload(unload) {
}

remidy::PluginBundlePool::~PluginBundlePool() {
    for (auto& entry : entries)
        unload(entry.second.moduleBundlePath, entry.second.module);
}

remidy::PluginBundlePool::RetentionPolicy remidy::PluginBundlePool::getRetentionPolicy() {
    return retentionPolicy;
}

void remidy::PluginBundlePool::setRetentionPolicy(RetentionPolicy value) {
    retentionPolicy = value;
}

void* remidy::PluginBundlePool::loadOrAddReference(std::filesystem::path &moduleBundlePath, bool* loadedAsNew) {
    //std::cerr << "AudioPluginLibraryPool::loadOrAddReference(" << moduleBundlePath.string() << ")" << std::endl;

    if (!loadedAsNew)
        return nullptr;
    *loadedAsNew = false;
    auto existing = entries.find(moduleBundlePath);
    if (existing != entries.end()) {
        existing->second.refCount++;
        return existing->second.module;
    }
    void* module{nullptr};
    EventLoop::runTaskOnMainThread([&] {
        auto result = load(moduleBundlePath, &module);
        if (result != StatusCode::OK)
            return;
        entries.emplace(moduleBundlePath, ModuleEntry{1, moduleBundlePath, module});
        *loadedAsNew = true;
    });
    return module;
}

remidy::StatusCode remidy::PluginBundlePool::removeReference(std::filesystem::path &moduleBundlePath) {
    //std::cerr << "AudioPluginLibraryPool::removeReference(" << libraryFile.string() << ")" << std::endl;

    auto entry = entries.find(moduleBundlePath);
    if (entry == entries.end())
        return StatusCode::BUNDLE_NOT_FOUND;
    if (--entry->second.refCount == 0 && retentionPolicy == RetentionPolicy::UnloadImmediately) {
        auto result = unload(entry->second.moduleBundlePath, entry->second.module);
        if (result != StatusCode::OK)
            return result;
        entries.erase(entry);
    }
    return StatusCode::OK;
}

