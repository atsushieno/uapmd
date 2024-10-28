#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#include <cpptrace/from_current.hpp>
#include <cpplocate/cpplocate.h>
#include <remidy/remidy.hpp>

// -------- instancing --------

class RemidyScan {

void doCreateInstance(remidy::AudioPluginFormat* format, remidy::PluginCatalogEntry catalogEntry) {
    auto entry = &catalogEntry;
    auto displayName = entry->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
    std::cerr << "  instantiating " << format->name() << " " << displayName << std::endl;

    format->createInstance(entry, [&](remidy::AudioPluginFormat::InvokeResult result) {
        remidy::setCurrentThreadNameIfPossible(std::format("remidy-scan.{}:{}", format->name(), entry->getMetadataProperty(remidy::PluginCatalogEntry::DisplayName)));
        bool successful = false;
        auto instance = std::move(result.instance);
        if (!instance)
            std::cerr << "  " << format->name() << ": Could not instantiate plugin " << displayName << ". Details: " << result.error << std::endl;
        else {
            remidy::AudioPluginInstance::ConfigurationRequest config{};
            auto code = instance->configure(config);
            if (code != remidy::StatusCode::OK)
                std::cerr << "  " << format->name() << ": " << displayName << " : configure() failed. Error code " << (int32_t) code << std::endl;
            else {
                code = instance->startProcessing();
                if (code != remidy::StatusCode::OK)
                    std::cerr << "  " << format->name() << ": " << displayName << " : startProcessing() failed. Error code " << (int32_t) code << std::endl;
                else {
                    // FIXME: use appropriate buses settings
                    uint32_t numAudioIn = instance->hasAudioInputs() ? 1 : 0;
                    uint32_t numAudioOut = instance->hasAudioOutputs() ? 1 : 0;
                    remidy::AudioProcessContext ctx{4096};
                    // FIXME: channel count is not precise.
                    if (numAudioIn > 0)
                        ctx.addAudioIn(2, 1024);
                    if (numAudioOut > 0)
                        ctx.addAudioOut(2, 1024);
                    ctx.frameCount(512);
                    for (uint32_t i = 0; i < numAudioIn; i++) {
                        // FIXME: channel count is not precise.
                        memcpy(ctx.audioIn(i)->getFloatBufferForChannel(0), (void*) "0123456789ABCDEF", 16);
                        memcpy(ctx.audioIn(i)->getFloatBufferForChannel(1), (void*) "FEDCBA9876543210", 16);
                    }
                    for (uint32_t i = 0; i < numAudioOut; i++) {
                        // FIXME: channel count is not precise.
                        memcpy(ctx.audioOut(i)->getFloatBufferForChannel(0), (void*) "02468ACE13579BDF", 16);
                        memcpy(ctx.audioOut(i)->getFloatBufferForChannel(1), (void*) "FDB97531ECA86420", 16);
                    }

                    code = instance->process(ctx);
                    if (code != remidy::StatusCode::OK)
                        std::cerr << "  " << format->name() << ": " << displayName << " : process() failed. Error code " << (int32_t) code << std::endl;
                    else
                        successful = true;

                    code = instance->stopProcessing();
                    if (code != remidy::StatusCode::OK)
                        std::cerr << "  " << format->name() << ": " << displayName << " : stopProcessing() failed. Error code " << (int32_t) code << std::endl;
                }
            }
        }
        if (successful)
            std::cerr << "  " << format->name() << ": Successfully instantiated and deleted " << displayName << std::endl;
    });
}

void testCreateInstance(remidy::AudioPluginFormat* format, remidy::PluginCatalogEntry* entry) {
    auto displayName = entry->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
    auto vendor = entry->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::VendorName);

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

    auto e = *entry; // copy
    if (forceMainThread || format->requiresUIThreadOn(entry) != remidy::None) {
        remidy::EventLoop::runTaskOnMainThread([format,e,this] {
            doCreateInstance(format, e);
        });
    }
    else
        doCreateInstance(format, e);
}

// -------- scanning --------

const char* APP_NAME= "remidy-scan";

    std::vector<std::string> vst3SearchPaths{};
    std::vector<std::string> lv2SearchPaths{};
    remidy::AudioPluginFormatVST3 vst3{vst3SearchPaths};
    remidy::AudioPluginFormatAU au{};
    remidy::AudioPluginFormatLV2 lv2{lv2SearchPaths};
    std::vector<remidy::AudioPluginFormat*> formats{&lv2, &au, &vst3};

auto filterByFormat(std::vector<remidy::PluginCatalogEntry*> entries, std::string format) {
    erase_if(entries, [format](remidy::PluginCatalogEntry* entry) { return entry->format() != format; });
    return entries;
}

int performPluginScanning() {

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
        if (!format->scanningMayBeSlow() || plugins.empty())
            for (auto& info : format->scanAllAvailablePlugins())
                if (!catalog.contains(info->format(), info->pluginId()))
                    catalog.add(std::move(info));
    }

    for (auto& format : formats) {
        auto plugins = filterByFormat(catalog.getPlugins(), format->name());

        int i= 0;
        for (auto info : plugins) {
            auto displayName = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
            auto vendor = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::VendorName);
            auto url = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::ProductUrl);
            std::cerr << "[" << ++i << "/" << plugins.size() << "] (" << info->format() << ") " << displayName << " : " << vendor << " (" << url << ")" << std::endl;
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

    std::cerr << "Completed ";
    for (auto& format : formats)
        std::cerr << format->name() << " " << std::endl;
    std::cerr << "formats.";

    return 0;
}

public:
int run(int argc, const char* argv[]) {
    int result{0};
    CPPTRACE_TRY {
        remidy::EventLoop::initializeOnUIThread();

        std::thread thread([&] {
            CPPTRACE_TRY {
                result = performPluginScanning();
                remidy::EventLoop::stop();
            } CPPTRACE_CATCH(const std::exception& e) {
                std::cerr << "Exception in main: " << e.what() << std::endl;
                cpptrace::from_current_exception().print();
            }
        });
        remidy::EventLoop::start();
    } CPPTRACE_CATCH(const std::exception& e) {
        std::cerr << "Exception in testCreateInstance: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
    }
    return result;
}

};

int main(int argc, const char* argv[]) {
    RemidyScan scanner{};
    scanner.run(argc, argv);
}
