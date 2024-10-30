
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
    auto displayName = entry->displayName();
    auto vendor = entry->vendorName();
    bool skip = false;

    // FIXME: implement blocklist
    if (displayName.starts_with("Firefly Synth 1.8.6 VST3"))
        skip = true;

    // FIXME: this should be unblocked

    // Plugin crashing!!!
    // Process finished with exit code 9
    if (displayName.starts_with("BYOD"))
        skip = true;

    // EXC_BAD_ACCESS
    if (format->name() == "AU" && displayName == "BioTek 2"
        || format->name() == "AU" && displayName == "DDSP Effect"
        || format->name() == "AU" && displayName == "DDSP Synth"
        || format->name() == "AU" && displayName == "RX 9 Music Rebalance"
        || format->name() == "AU" && displayName == "RX 9 Spectral Editor"
        || format->name() == "AU" && displayName == "Absynth 5 MFX"
        || format->name() == "AU" && displayName == "Reaktor 6 MFX"
        || format->name() == "AU" && displayName == "Reaktor 6 MIDIFX"
        || format->name() == "AU" && displayName == "Absynth 5"
        || format->name() == "AU" && displayName == "Reaktor 6"
        || format->name() == "AU" && displayName == "Ozone 9"
        || format->name() == "AU" && displayName == "BioTek"
        || format->name() == "AU" && displayName == "BioTek 2"
        || format->name() == "AU" && displayName == "Plugin Buddy"
        || format->name() == "AU" && displayName == "Collective"
        )
        skip = true;

    // goes unresponsive at AudioUnitRender()
    if (format->name() == "AU" && displayName.starts_with("AUSpeechSynthesis"))
        skip = true;

    // This prevents IEditController and IAudioProcessor inter-connection.
    //if (format->name() == "VST3" && displayName.starts_with("RX 8 Breath Control"))
    //    skip = true;

    // (goes unresponsive, only on debug builds, but still matters)
    if (format->name() == "VST3" && displayName == "Vital"
        || format->name() == "VST3" && vendor == "Tracktion"
        || format->name() == "VST3" && displayName == "Neutron 3 Visual Mixer"
        || format->name() == "VST3" && displayName == "Ozone 9 Dynamics"
        || format->name() == "VST3" && displayName == "Stutter Edit 2"
        || format->name() == "VST3" && displayName == "RX 9 De-plosive"
        || format->name() == "VST3" && displayName == "RX 9 De-crackle"
        || format->name() == "VST3" && displayName == "RX 9 De-hum"
        || format->name() == "VST3" && displayName == "RX 9 Voice De-noise"
        || format->name() == "VST3" && displayName == "RX 9 De-reverb"
        || format->name() == "VST3" && displayName == "RX 9 Connect"
        || format->name() == "VST3" && displayName == "Ozone 9 Equalizer"
        || format->name() == "VST3" && displayName == "Ozone 9 Master Rebalance"
        || format->name() == "VST3" && displayName == "Ozone 9 Spectral Shaper"
        || format->name() == "VST3" && displayName == "Ozone 9 Imager"
        || format->name() == "VST3" && displayName == "Ozone 9 Match EQ"
        || format->name() == "VST3" && displayName == "Ozone 9 Maximizer"
        || format->name() == "VST3" && displayName == "Neutron 3 Sculptor"
        || format->name() == "VST3" && displayName == "Vienna Synchron Player Surround"
        )
        skip = true;

    // They can be instantiated but in the end they cause: Process finished with exit code 134 (interrupted by signal 6:SIGABRT)
    if (format->name() == "VST3" && displayName.starts_with("Battery"))
        skip = true;
    if (format->name() == "VST3" && displayName.starts_with("Kontakt"))
        skip = true;
    if (format->name() == "AU" && displayName.starts_with("FL Studio"))
        skip = true;
    if (format->name() == "VST3" && displayName == "RX 9 Monitor")
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

bool remidy::PluginScanner::createInstanceOnUIThread(AudioPluginFormat *format, PluginCatalogEntry* entry) {
    auto displayName = entry->displayName();
    auto vendor = entry->vendorName();
    bool forceMainThread =
        format->name() == "AU" && displayName == "FL Studio"
        || format->name() == "AU" && displayName == "AUSpeechSynthesis"
        // QObject::killTimer: Timers cannot be stopped from another thread
        // QObject::~QObject: Timers cannot be stopped from another thread
        || format->name() == "AU" && displayName == "Massive X"
        // [threadmgrsupport] _TSGetMainThread_block_invoke():Main thread potentially initialized incorrectly, cf <rdar://problem/67741850>
        || format->name() == "AU" && displayName == "Bite"
        // WARNING: QApplication was not created in the main() thread
        || format->name() == "AU" && displayName == "Raum"
        || format->name() == "AU" && displayName == "Guitar Rig 6 FX"
        || format->name() == "AU" && displayName == "Guitar Rig 6 MFX"
    ;
    return forceMainThread || format->requiresUIThreadOn(entry) != AudioPluginUIThreadRequirement::None;
}
