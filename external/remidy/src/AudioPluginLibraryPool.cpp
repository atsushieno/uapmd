
#include "AudioPluginLibraryPool.hpp"

remidy::AudioPluginLibraryPool::AudioPluginLibraryPool(
    std::function<remidy_status_t(std::filesystem::path& libraryFile, void** module)>& load,
    std::function<remidy_status_t(std::filesystem::path& libraryFile, void* module)>& unload
) : load(load), unload(unload) {
}

remidy::AudioPluginLibraryPool::~AudioPluginLibraryPool() {
    for (auto& entry : entries)
        unload(entry.second.libraryFile, entry.second.module);
}

remidy::AudioPluginLibraryPool::RetentionPolicy remidy::AudioPluginLibraryPool::getRetentionPolicy() {
    return retentionPolicy;
}

void remidy::AudioPluginLibraryPool::setRetentionPolicy(RetentionPolicy value) {
    retentionPolicy = value;
}

void* remidy::AudioPluginLibraryPool::loadOrAddReference(std::filesystem::path &libraryFile) {
    auto existing = entries.find(libraryFile);
    if (existing != entries.end()) {
        existing->second.refCount++;
        return &existing->second.module;
    }
    void* lib{nullptr};
    auto result = load(libraryFile, &lib);
    if (result != 0)
        return nullptr;
    entries.emplace(libraryFile, ModuleEntry{1, libraryFile, lib});
    return lib;
}

remidy_status_t remidy::AudioPluginLibraryPool::removeReference(std::filesystem::path &libraryFile) {
    auto entry = entries.find(libraryFile);
    if (entry == entries.end())
        // FIXME: define error status codes
        return -1;
    if (--entry->second.refCount == 0 && retentionPolicy == RetentionPolicy::UnloadImmediately) {
        auto result = unload(entry->second.libraryFile, entry->second.module);
        if (result != 0)
            return result;
        entries.erase(entry);
    }
    // FIXME: define error status codes
    return 0;
}
