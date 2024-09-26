
#include "AudioPluginLibraryPool.hpp"

#include <iostream>

remidy::AudioPluginLibraryPool::AudioPluginLibraryPool(
    std::function<remidy_status_t(std::filesystem::path& moduleBundlePath, void** module)>& load,
    std::function<remidy_status_t(std::filesystem::path& moduleBundlePath, void* module)>& unload
) : load(load), unload(unload) {
}

remidy::AudioPluginLibraryPool::~AudioPluginLibraryPool() {
    for (auto& entry : entries)
        unload(entry.second.moduleBundlePath, entry.second.module);
}

remidy::AudioPluginLibraryPool::RetentionPolicy remidy::AudioPluginLibraryPool::getRetentionPolicy() {
    return retentionPolicy;
}

void remidy::AudioPluginLibraryPool::setRetentionPolicy(RetentionPolicy value) {
    retentionPolicy = value;
}

void* remidy::AudioPluginLibraryPool::loadOrAddReference(std::filesystem::path &moduleBundlePath) {
    //std::cerr << "AudioPluginLibraryPool::loadOrAddReference(" << moduleBundlePath.string() << ")" << std::endl;

    auto existing = entries.find(moduleBundlePath);
    if (existing != entries.end()) {
        existing->second.refCount++;
        return existing->second.module;
    }
    void* module{nullptr};
    auto result = load(moduleBundlePath, &module);
    if (result != 0)
        return nullptr;
    entries.emplace(moduleBundlePath, ModuleEntry{1, moduleBundlePath, module});
    return module;
}

remidy_status_t remidy::AudioPluginLibraryPool::removeReference(std::filesystem::path &moduleBundlePath) {
    //std::cerr << "AudioPluginLibraryPool::removeReference(" << libraryFile.string() << ")" << std::endl;

    auto entry = entries.find(moduleBundlePath);
    if (entry == entries.end())
        // FIXME: define error status codes
        return -1;
    if (--entry->second.refCount == 0 && retentionPolicy == RetentionPolicy::UnloadImmediately) {
        auto result = unload(entry->second.moduleBundlePath, entry->second.module);
        if (result != 0)
            return result;
        entries.erase(entry);
    }
    // FIXME: define error status codes
    return 0;
}
