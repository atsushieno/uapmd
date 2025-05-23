
#include "remidy-tooling/PluginScanTool.hpp"
#include "cpplocate/cpplocate.h"

const char* TOOLING_DIR_NAME= "remidy-tooling";

remidy_tooling::PluginScanTool::PluginScanTool() {
    auto dir = cpplocate::localDir(TOOLING_DIR_NAME);
    plugin_list_cache_file = dir.empty() ? std::filesystem::path{""} : std::filesystem::path{dir}.append(
            "plugin-list-cache.json");
}

int remidy_tooling::PluginScanTool::performPluginScanning() {
    return performPluginScanning(plugin_list_cache_file);
}

int remidy_tooling::PluginScanTool::performPluginScanning(std::filesystem::path& pluginListCacheFile) {
    if (std::filesystem::exists(pluginListCacheFile)) {
        catalog.load(pluginListCacheFile);
        plugin_list_cache_file = pluginListCacheFile;
    }

    // build catalog
    for (auto& format : formats()) {
        auto plugins = filterByFormat(catalog.getPlugins(), format->name());
        if (!format->scanning()->scanningMayBeSlow() || plugins.empty())
            for (auto& info : format->scanning()->scanAllAvailablePlugins())
                if (!catalog.contains(info->format(), info->pluginId()))
                    catalog.add(std::move(info));
    }

    return 0;
}


bool remidy_tooling::PluginScanTool::safeToInstantiate(PluginFormat* format, PluginCatalogEntry *entry) {
    auto displayName = entry->displayName();
    auto vendor = entry->vendorName();
    bool skip = false;

    // FIXME: implement blocklist

    // FIXME: this should be unblocked

    // EXC_BAD_ACCESS (Massive X used to work though...)
    if (format->name() == "AU" && displayName == "Massive X"
        || format->name() == "AU" && displayName == "BioTek"
        || format->name() == "AU" && displayName == "BioTek 2"
        || format->name() == "AU" && displayName == "Collective"
        )
        skip = true;
    // It depends on unimplemented perform_edit() and restart_component(). Results in unresponsiveness.
    if (format->name() == "VST3" && displayName == "Massive X")
        skip = true;

    // This prevents IEditController and IAudioProcessor inter-connection.
    //if (format->name() == "VST3" && displayName.starts_with("RX 8 Breath Control"))
    //    skip = true;

    // (goes unresponsive, only on debug builds, but still matters)
    if (format->name() == "VST3" && displayName == "Vienna Synchron Player"
        || format->name() == "VST3" && displayName == "Vienna Synchron Player Surround"
        || format->name() == "AU" && displayName == "Vienna Synchron Player"
        )
        skip = true;

    // They can be instantiated but in the end they cause: Process finished with exit code 134 (interrupted by signal 6:SIGABRT)
    /*if (format->name() == "VST3" && displayName.starts_with("Battery")
        || format->name() == "VST3" && displayName.starts_with("Kontakt"))
        || format->name() == "VST3" && displayName == "RX 9 Monitor"
        )
        skip = true;*/
    if (format->name() == "AU" && displayName == "FL Studio")
        skip = true;

#if __APPLE__ || WIN32
    // they generate *.dylib in .ttl, but the library is *.so...
    if (format->name() == "LV2" && displayName == "AIDA-X")
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

bool remidy_tooling::PluginScanTool::shouldCreateInstanceOnUIThread(PluginFormat *format, PluginCatalogEntry* entry) {
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
        || format->name() == "AU" && displayName == "Vienna Synchron Player"
        // EXC_BAD_ACCESS, maybe because they are JUCE
        || format->name() == "AU" && displayName == "DDSP Effect"
        || format->name() == "AU" && displayName == "DDSP Synth"
        || format->name() == "AU" && displayName == "Plugin Buddy"
        // EXC_BAD_ACCESS, not sure why
        || format->name() == "AU" && displayName == "RX 9 Music Rebalance"
        || format->name() == "AU" && displayName == "RX 9 Spectral Editor"
        || format->name() == "AU" && displayName == "Ozone 9"
        || format->name() == "AU" && displayName == "Absynth 5"
        || format->name() == "AU" && displayName == "Absynth 5 MFX"
        || format->name() == "AU" && displayName == "FM8"
        || format->name() == "AU" && displayName == "Reaktor 6"
        || format->name() == "AU" && displayName == "Reaktor 6 MFX"
        || format->name() == "AU" && displayName == "Reaktor 6 MIDIFX"
        // Some JUCE plugins are NOT designed to work in thread safe manner (JUCE behaves like VST3).
        // (Use AddressSanitizer to detect such failing ones.)
        //
        // (It may not be everything from Tracktion, but there are too many #T* plugins.)
        || format->name() == "AU" && vendor == "Tracktion"
    ;
    return forceMainThread || format->requiresUIThreadOn(entry) != PluginUIThreadRequirement::None;
}
