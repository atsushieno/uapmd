
#if !ANDROID
#include <cpplocate/cpplocate.h>
#endif
#include "remidy-tooling/PluginScanTool.hpp"

const char* TOOLING_DIR_NAME= "remidy-tooling";

remidy_tooling::PluginScanTool::PluginScanTool() {
#if ANDROID
    std::filesystem::path dir{};
#else
    auto dir = cpplocate::localDir(TOOLING_DIR_NAME);
#endif
    plugin_list_cache_file = dir.empty() ? std::filesystem::path{""} : std::filesystem::path{dir}.append(
            "plugin-list-cache.json");

#if ANDROID
    aap = remidy::PluginFormatAAP::create();
    formats_ = { aap.get() };
#else
    vst3 = remidy::PluginFormatVST3::create(vst3SearchPaths);
    lv2 = remidy::PluginFormatLV2::create(lv2SearchPaths);
    clap = remidy::PluginFormatCLAP::create(clapSearchPaths);
#if __APPLE__
    //au = remidy::PluginFormatAU::create();
    au = remidy::PluginFormatAU::create();
#endif

    formats_ = {
        clap.get(),
        lv2.get(),
#if __APPLE__
        au.get(),
#endif
        vst3.get()
    };
#endif
}

int remidy_tooling::PluginScanTool::performPluginScanning(bool requireFastScanning) {
    return performPluginScanning(requireFastScanning, plugin_list_cache_file);
}

int remidy_tooling::PluginScanTool::performPluginScanning(bool requireFastScanning, std::filesystem::path& pluginListCacheFile) {
    if (std::filesystem::exists(pluginListCacheFile)) {
        catalog.load(pluginListCacheFile);
        plugin_list_cache_file = pluginListCacheFile;
    }

    // build catalog
    auto savedCwd = std::filesystem::current_path();
    for (auto& format : formats()) {
        auto plugins = filterByFormat(catalog.getPlugins(), format->name());
        if (!format->scanning()->scanningMayBeSlow() || plugins.empty())
            for (auto& info : format->scanning()->scanAllAvailablePlugins(requireFastScanning))
                if (!catalog.contains(info->format(), info->pluginId()))
                    catalog.add(std::move(info));
    }
    std::filesystem::current_path(savedCwd);

    return 0;
}


bool remidy_tooling::PluginScanTool::safeToInstantiate(PluginFormat* format, PluginCatalogEntry *entry) {
    auto displayName = entry->displayName();
    auto vendor = entry->vendorName();
    bool skip = false;

    // FIXME: implement blocklist

    // Not sure when it started, but their AU version stalls while instantiating.
    // Note that they can be instantiated just fine. Maybe just instancing them among many.
    if (format->name() == "AU" && vendor == "Tracktion")
        skip = true;

    // They somehow trigger scanner crashes.
    // Note that they can be instantiated just fine. Maybe just instancing them among many.
    if (format->name() == "CLAP" && displayName == "Floe")
        skip = true;
    if (format->name() == "CLAP" && displayName == "Zebrify")
        skip = true;
    if (format->name() == "CLAP" && displayName == "Zebralette3")
        skip = true;
    if (format->name() == "CLAP" && displayName == "ysfx-s instrument")
        skip = true;
    if (format->name() == "CLAP" && displayName == "ysfx-s FX")
        skip = true;
    if (format->name() == "CLAP" && displayName == "Ragnarok2")
        skip = true;

    if (format->name() == "AU" && displayName == "RX 9 Monitor")
        skip = true;

#if __APPLE__ || WIN32
    // they generate *.dylib in .ttl, but the library is *.so...
    if (format->name() == "LV2" && displayName == "AIDA-X")
        skip = true;
    // they generate *.so in .ttl, but the library is *.dylib...
    if (format->name() == "LV2" && displayName.starts_with("sfizz"))
        skip = true;
#endif

    return !skip;
}

bool remidy_tooling::PluginScanTool::shouldCreateInstanceOnUIThread(PluginFormat *format, PluginCatalogEntry* entry) {
    auto displayName = entry->displayName();
    auto vendor = entry->vendorName();
    bool forceMainThread =
        // not sure why
        format->name() == "AU" && displayName == "FL Studio"
        || format->name() == "AU" && displayName == "SINE Player"
        || format->name() == "AU" && displayName == "AUSpeechSynthesis"
        // QObject::killTimer: Timers cannot be stopped from another thread
        // QObject::~QObject: Timers cannot be stopped from another thread
        || format->name() == "AU" && displayName.contains("Massive X")
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
        || format->name() == "AU" && displayName == "Synthesizer V Studio Plugin"
        // EXC_BAD_ACCESS, not sure why
        || format->name() == "AU" && displayName == "BBC Symphony Orchestra"
        || format->name() == "AU" && displayName == "LABS"
        || format->name() == "AU" && displayName == "BFD Player"
        || format->name() == "AU" && displayName == "RX 9 Music Rebalance"
        || format->name() == "AU" && displayName == "RX 9 Spectral Editor"
        || format->name() == "AU" && displayName == "Ozone 9"
        || format->name() == "AU" && displayName == "Absynth 5"
        || format->name() == "AU" && displayName == "Absynth 5 MFX"
        || format->name() == "AU" && displayName == "FM8"
        || format->name() == "AU" && displayName == "Massive X"
        || format->name() == "AU" && displayName == "Reaktor 6"
        || format->name() == "AU" && displayName == "Reaktor 6 MFX"
        || format->name() == "AU" && displayName == "Reaktor 6 MIDIFX"
        || format->name() == "AU" && displayName == "Kontakt 8"
        // Some JUCE plugins are NOT designed to work in thread safe manner (JUCE behaves like VST3).
        // (Use AddressSanitizer to detect such failing ones.)
        //
        // (It may not be everything from Tracktion, but there are too many #T* plugins.)
        || format->name() == "AU" && vendor == "Tracktion"
    ;
    return forceMainThread || format->requiresUIThreadOn(entry) != PluginUIThreadRequirement::None;
}
