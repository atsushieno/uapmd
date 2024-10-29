#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#include <cpptrace/from_current.hpp>
#include <cpplocate/cpplocate.h>
#include <remidy/remidy.hpp>
#include "PluginScanner.hpp"

// -------- instancing --------
const char* APP_NAME= "remidy-scan";

remidy::PluginScanner scanner{APP_NAME};

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
    auto e = *entry; // copy
    if (scanner.createInstanceOnUIThread(format, entry)) {
        remidy::EventLoop::runTaskOnMainThread([format,e,this] {
            doCreateInstance(format, e);
        });
    }
    else
        doCreateInstance(format, e);
}

int testInstancing() {
    for (auto& format : scanner.formats) {
        auto plugins = scanner.filterByFormat(scanner.catalog.getPlugins(), format->name());

        int i= 0;
        for (auto info : plugins) {
            auto displayName = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
            auto vendor = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::VendorName);
            auto url = info->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::ProductUrl);

            std::cerr << "[" << ++i << "/" << plugins.size() << "] (" << info->format() << ") " << displayName << " : " << vendor << " (" << url << ")" << std::endl;
            if (scanner.safeToInstantiate(format, info))
                testCreateInstance(format, info);
            else
                std::cerr << "  Plugin (" << info->format() << ") " << displayName << " is skipped." << std::endl;
        }
    }

    return 0;
}

public:
int run(int argc, const char* argv[]) {
    int result{0};
    CPPTRACE_TRY {
        result = scanner.performPluginScanning();

        remidy::EventLoop::initializeOnUIThread();
        std::thread thread([&] {
            CPPTRACE_TRY {
                result = testInstancing();
                remidy::EventLoop::stop();

                scanner.savePluginListCache();

                std::cerr << "Completed " << std::endl;
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
