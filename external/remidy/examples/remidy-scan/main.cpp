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
            remidy::AudioPluginInstance::ConfigurationRequest config{};
            auto code = instance->configure(config);
            if (code != remidy::StatusCode::OK)
                std::cerr << format->name() << ": " << displayName << " : configure() failed. Error code " << (int32_t) code << std::endl;
            else {
                code = instance->startProcessing();
                if (code != remidy::StatusCode::OK)
                    std::cerr << format->name() << ": " << displayName << " : startProcessing() failed. Error code " << (int32_t) code << std::endl;
                else {
                    // FIXME: use appropriate buses settings
                    uint32_t numAudioIn = instance->hasAudioInputs() ? 1 : 0;
                    uint32_t numAudioOut = instance->hasAudioOutputs() ? 1 : 0;
                    remidy::AudioProcessContext ctx{4096};
                    if (numAudioIn > 0)
                        ctx.addAudioIn(2, 1024);
                    if (numAudioOut > 0)
                        ctx.addAudioOut(2, 1024);
                    ctx.frameCount(512);
                    if (ctx.audioInBusCount() > 0) {
                        memcpy(ctx.audioIn(0)->getFloatBufferForChannel(0), (void*) "0123456789ABCDEF", 16);
                        memcpy(ctx.audioIn(0)->getFloatBufferForChannel(1), (void*) "FEDCBA9876543210", 16);
                    }
                    if (ctx.audioOutBusCount() > 0) {
                        memcpy(ctx.audioOut(0)->getFloatBufferForChannel(0), (void*) "02468ACE13579BDF", 16);
                        memcpy(ctx.audioOut(0)->getFloatBufferForChannel(1), (void*) "FDB97531ECA86420", 16);
                    }
                    instance->process(ctx);

                    code = instance->stopProcessing();
                    if (code != remidy::StatusCode::OK)
                        std::cerr << format->name() << ": " << displayName << " : stopProcessing() failed. Error code " << (int32_t) code << std::endl;
                    else
                    std::cerr << format->name() << ": Successfully instantiated and deleted " << displayName << std::endl;
                }
            }
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
    remidy::EventLoop::initializeOnUIThread();

    std::vector<std::string> vst3SearchPaths{};
    std::vector<std::string> lv2SearchPaths{};
    remidy::AudioPluginFormatVST3 vst3{vst3SearchPaths};
    remidy::AudioPluginFormatAU au{};
    remidy::AudioPluginFormatLV2 lv2{lv2SearchPaths};
    auto formats = std::vector<remidy::AudioPluginFormat*>{&lv2/*, &vst3*/, &au};

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
            bool skip = false;

            // FIXME: implement blocklist
            if (displayName.starts_with("Firefly Synth 1.8.6 VST3"))
                skip = true;

            // FIXME: this should be unblocked

            // goes unresponsive at AudioUnitRender().
            if (format->name() == "AU" && vendor == "Tracktion")
                skip = true;

            // They can be instantiated but in the end they cause: Process finished with exit code 134 (interrupted by signal 6:SIGABRT)
            if (format->name() == "VST3" && displayName.starts_with("Battery"))
                skip = true;
            if (format->name() == "VST3" && displayName.starts_with("Kontakt"))
                skip = true;
            if (format->name() == "AU" && displayName.starts_with("FL Studio"))
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

            if (skip)
                std::cerr << "  Plugin (" << info->format() << ") " << displayName << " is skipped." << std::endl;
            else
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
