#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#include <cpplocate/cpplocate.h>
#include <remidy/remidy.hpp>

void testCreateInstance(remidy::AudioPluginFormat* format, remidy::PluginCatalogEntry* pluginId) {
    auto displayName = pluginId->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
    std::cerr << "instantiating " << displayName << std::endl;
    std::atomic<bool> completed{false};

    format->createInstance(pluginId, [&](remidy::AudioPluginFormat::InvokeResult result) {
        auto instance = std::move(result.instance);
        if (!instance)
            std::cerr << format->name() << ": Could not instantiate plugin " << displayName << ". Details: " << result.error << std::endl;
        else {
            auto code = instance->configure(48000);
            if (code != remidy::OK)
                std::cerr << format->name() << ": " << displayName << " : configure() failed. Error code " << code << std::endl;
            else
                std::cerr << format->name() << ": Successfully instantiated and deleted " << displayName << std::endl;
        }
        completed = true;
        completed.notify_one();
    });
    completed.wait(false);
}

const char* APP_NAME= "remidy-scan";

auto filterByFormat(std::vector<remidy::PluginCatalogEntry*> entries, std::string format) {
    erase_if(entries, [format](remidy::PluginCatalogEntry* entry) { return entry->format() != format; });
    return entries;
}

int main(int argc, const char * argv[]) {
    std::vector<std::string> vst3SearchPaths{};
    std::vector<std::string> lv2SearchPaths{};
    remidy::AudioPluginFormatVST3 vst3{vst3SearchPaths};
    remidy::AudioPluginFormatAU au{};
    remidy::AudioPluginFormatLV2 lv2{lv2SearchPaths};
    auto formats = std::vector<remidy::AudioPluginFormat*>{&lv2, &au, &vst3};

    remidy::PluginCatalog catalog{};
    auto dir = cpplocate::localDir(APP_NAME);
    auto pluginListCacheFile = dir.empty() ?  std::filesystem::path{""} : std::filesystem::path{dir}.append("plugin-list-cache.json");
    if (std::filesystem::exists(pluginListCacheFile)) {
        catalog.load(pluginListCacheFile);
        std::cerr << "Loaded plugin list cache from " << pluginListCacheFile << std::endl;
    }

    // build catalog
    for (auto& format : formats) {
        auto plugins = filterByFormat(catalog.getPlugins(), format->name());
        if (!format->hasPluginListCache() || plugins.empty())
            for (auto& info : format->scanAllAvailablePlugins())
                if (!catalog.contains(info->format(), info->pluginId()))
                catalog.add(std::move(info));
    }

    for (auto& format : formats) {
        auto plugins = filterByFormat(catalog.getPlugins(), format->name());

        int i= 0;
        for (auto& info : plugins) {
            auto displayName = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
            auto vendor = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::VendorName);
            auto url = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::ProductUrl);
            std::cerr << "[" << ++i << "/" << plugins.size() << "] (" << info->format() << ") " << displayName << " : " << vendor << " (" << url << ")" << std::endl;

            // FIXME: implement blocklist
            if (displayName.starts_with("Firefly Synth 1.8.6 VST3"))
                continue;

            // FIXME: this should be unblocked

            // They can be instantiated but in the end they cause: Process finished with exit code 134 (interrupted by signal 6:SIGABRT)
            if (format->name() == "VST3" && displayName.starts_with("Battery"))
                continue;
            if (format->name() == "VST3" && displayName.starts_with("Kontakt"))
                continue;
            if (format->name() == "AU" && displayName.starts_with("FL Studio"))
                continue;
#if __APPLE__ || WIN32
            // they generate *.dylib in .ttl, but the library is *.so...
            if (format->name() == "LV2" && displayName.starts_with("AIDA-X"))
                continue;
            // they generate *.so in .ttl, but the library is *.dylib...
            if (format->name() == "LV2" && displayName.starts_with("sfizz"))
                continue;
#endif
            // causes JUCE leak detector exception (not on OPNplug-AE though)
            //if (!displayName.starts_with("ADLplug-AE"))
            //    continue;

            testCreateInstance(format, info);
        }
    }

    if (!pluginListCacheFile.empty()) {
        catalog.save(pluginListCacheFile);
        std::cerr << "Saved plugin list cache as " << pluginListCacheFile << std::endl;
    }

    std::cerr << "Completed." << std::endl;

    return 0;
}
