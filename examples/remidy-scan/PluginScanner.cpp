
#include "PluginScanner.hpp"


int remidy::PluginScanner::performPluginScanning()  {
    auto dir = cpplocate::localDir(app_name);
    pluginListCacheFile = dir.empty() ?  std::filesystem::path{""} : std::filesystem::path{dir}.append("plugin-list-cache.json");
    if (std::filesystem::exists(pluginListCacheFile)) {
        catalog.load(pluginListCacheFile);
        //std::cerr << "Loaded plugin list cache from " << pluginListCacheFile << std::endl;
    }

    // build catalog
    for (auto& format : formats) {
        auto plugins = filterByFormat(catalog.getPlugins(), format->name());
        if (!format->scanningMayBeSlow() || plugins.empty())
            for (auto& info : format->scanAllAvailablePlugins())
                if (!catalog.contains(info->format(), info->pluginId()))
                    catalog.add(std::move(info));
    }

    return 0;
}


bool remidy::PluginScanner::safeToInstantiate(AudioPluginFormat* format, PluginCatalogEntry *entry) {
    auto displayName = entry->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
    auto vendor = entry->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::VendorName);
    auto url = entry->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::ProductUrl);
    bool skip = false;

    // FIXME: implement blocklist
    if (displayName.starts_with("Firefly Synth 1.8.6 VST3"))
        skip = true;

    // FIXME: this should be unblocked

    // Plugin crashing!!!
    // Process finished with exit code 9
    if (displayName.starts_with("BYOD"))
        skip = true;

    // goes unresponsive at AudioUnitRender()
    if (format->name() == "AU" && displayName.starts_with("AUSpeechSynthesis"))
        skip = true;

    // They can be instantiated but in the end they cause: Process finished with exit code 134 (interrupted by signal 6:SIGABRT)
    if (format->name() == "VST3" && displayName.starts_with("Battery"))
        skip = true;
    if (format->name() == "VST3" && displayName.starts_with("Kontakt"))
        skip = true;
    if (format->name() == "AU" && displayName.starts_with("FL Studio"))
        skip = true;

    // likewise, but SIGTRAP, not SIGABRT.
    // They are most likely causing blocked operations.
    // (I guess they are caught as SIGTRAP because they run on the main thread. Otherwise their thread would be just stuck forever.)
    if (displayName == "Vienna Synchron Player") // AU and VST3
        skip = true;
    if (displayName.starts_with("Massive X")) // AU and VST3
        skip = true;
    if (format->name() == "AU" && displayName == "#TAuto Filter")
        skip = true;
    if (displayName == "Vienna Ensemble")
        skip = true;

#if __APPLE__ || WIN32
    // they generate *.dylib in .ttl, but the library is *.so...
    if (format->name() == "LV2" && displayName.starts_with("AIDA-X"))
        skip = true;
    // they generate *.so in .ttl, but the library is *.dylib...
    if (format->name() == "LV2" && displayName.starts_with("sfizz"))
        skip = true;
#endif
    // causes JUCE leak detector exception (not on OPNplug-AE though)
    //if (!displayName.starts_with("ADLplug-AE"))
    //    continue;

    return !skip;
}

bool remidy::PluginScanner::createInstanceOnUIThread(AudioPluginFormat *format, PluginCatalogEntry *entry) {
    auto displayName = entry->getMetadataProperty(PluginCatalogEntry::MetadataPropertyID::DisplayName);
    auto vendor = entry->getMetadataProperty(PluginCatalogEntry::MetadataPropertyID::VendorName);
    bool forceMainThread =
        format->name() == "AU" && vendor == "Tracktion"
        || format->name() == "AU" && displayName == "AUSpeechSynthesis"
        || format->name() == "AU" && displayName == "FL Studio"
        // QObject::killTimer: Timers cannot be stopped from another thread
        // QObject::~QObject: Timers cannot be stopped from another thread
        || format->name() == "AU" && displayName == "Massive X"
        // [threadmgrsupport] _TSGetMainThread_block_invoke():Main thread potentially initialized incorrectly, cf <rdar://problem/67741850>
        || format->name() == "AU" && displayName == "Bite"
        // WARNING: QApplication was not created in the main() thread
        || format->name() == "AU" && displayName == "Guitar Rig 6 MFX"
    ;
    return forceMainThread || format->requiresUIThreadOn(entry) != AudioPluginUIThreadRequirement::None;
}
