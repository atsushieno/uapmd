
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

void* remidy::PluginBundlePool::loadOrAddReference(std::filesystem::path &moduleBundlePath) {
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

remidy::StatusCode remidy::PluginBundlePool::removeReference(std::filesystem::path &moduleBundlePath) {
    //std::cerr << "AudioPluginLibraryPool::removeReference(" << libraryFile.string() << ")" << std::endl;

    auto entry = entries.find(moduleBundlePath);
    if (entry == entries.end())
        return StatusCode::BUNDLE_NOT_FOUND;
    if (--entry->second.refCount == 0 && retentionPolicy == RetentionPolicy::UnloadImmediately) {
        auto result = unload(entry->second.moduleBundlePath, entry->second.module);
        if (result != 0)
            return result;
        entries.erase(entry);
    }
    return StatusCode::OK;
}

std::vector<remidy::PluginCatalogEntry*> remidy::PluginCatalog::getPlugins() {
    std::vector<remidy::PluginCatalogEntry *> ret{};
    ret.reserve(list.size());
    for (auto& entry : list)
        ret.emplace_back(entry.get());
    return ret;
}

void remidy::PluginCatalog::add(std::unique_ptr<PluginCatalogEntry> entry) {
    list.emplace_back(std::move(entry));
}

void remidy::PluginCatalog::clear() {
    list.clear();
}

void remidy::PluginCatalog::load(std::filesystem::path path) {
    throw std::runtime_error("remidy::PluginCatalog::load() is not implemented.");
}

void remidy::PluginCatalog::save(std::filesystem::path path) {
    throw std::runtime_error("remidy::PluginCatalog::save() is not implemented.");
}

