
#include "AudioPluginLibraryPool.hpp"

#include <iostream>

remidy::AudioPluginLibraryPool::AudioPluginLibraryPool(
    std::function<remidy_status_t(std::filesystem::path& vst3Dir, void** module)>& load,
    std::function<remidy_status_t(std::filesystem::path& vst3Dir, void* module)>& unload
) : load(load), unload(unload) {
}

remidy::AudioPluginLibraryPool::~AudioPluginLibraryPool() {
    for (auto& entry : entries)
        unload(entry.second.vst3Dir, entry.second.module);
}

remidy::AudioPluginLibraryPool::RetentionPolicy remidy::AudioPluginLibraryPool::getRetentionPolicy() {
    return retentionPolicy;
}

void remidy::AudioPluginLibraryPool::setRetentionPolicy(RetentionPolicy value) {
    retentionPolicy = value;
}

void* remidy::AudioPluginLibraryPool::loadOrAddReference(std::filesystem::path &vst3Dir) {
    //std::cerr << "AudioPluginLibraryPool::loadOrAddReference(" << vst3Dir.string() << ")" << std::endl;

    auto existing = entries.find(vst3Dir);
    if (existing != entries.end()) {
        existing->second.refCount++;
        return existing->second.module;
    }
    void* lib{nullptr};
    auto result = load(vst3Dir, &lib);
    if (result != 0)
        return nullptr;
    entries.emplace(vst3Dir, ModuleEntry{1, vst3Dir, lib});
    return lib;
}

remidy_status_t remidy::AudioPluginLibraryPool::removeReference(std::filesystem::path &libraryFile) {
    //std::cerr << "AudioPluginLibraryPool::removeReference(" << libraryFile.string() << ")" << std::endl;

    auto entry = entries.find(libraryFile);
    if (entry == entries.end())
        // FIXME: define error status codes
        return -1;
    if (--entry->second.refCount == 0 && retentionPolicy == RetentionPolicy::UnloadImmediately) {
        auto result = unload(entry->second.vst3Dir, entry->second.module);
        if (result != 0)
            return result;
        entries.erase(entry);
    }
    // FIXME: define error status codes
    return 0;
}
