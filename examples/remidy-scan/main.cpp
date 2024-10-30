#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#include <cpptrace/from_current.hpp>
#include <cpplocate/cpplocate.h>
#include <remidy/remidy.hpp>
#include "PluginScanner.hpp"
#include "PluginInstancing.hpp"

// -------- instancing --------
const char* APP_NAME= "remidy-scan";

remidy::PluginScanner scanner{APP_NAME};

class RemidyScan {

int testInstancing() {
    for (auto format : scanner.formats) {
        auto plugins = scanner.filterByFormat(scanner.catalog.getPlugins(), format->name());

        int i= 0;
        for (auto info : plugins) {
            auto displayName = info->displayName();
            auto vendor = info->vendorName();
            auto url = info->productUrl();

            std::cerr << "[" << ++i << "/" << plugins.size() << "] (" << info->format() << ") " << displayName << " : " << vendor << " (" << url << ")" << std::endl;

            if (!scanner.safeToInstantiate(format, info)) {
                std::cerr << "  Plugin (" << info->format() << ") " << displayName << " is skipped." << std::endl;
                continue;
            }
            bool successful = false;
            {
                // scoped object
                remidy::PluginInstancing instancing{scanner, format, info};
                // ...
                // you could adjust configuration here
                // ...
                std::atomic<bool> waitHandle{false};
                instancing.makeAlive([&](std::string err) {
                    waitHandle.store(true);
                    waitHandle.notify_all();
                });
                while (!waitHandle.load())
                    std::this_thread::yield();

                std::cerr << "  " << format->name() << ": Successfully configured " << displayName << ". Instantiating now..." << std::endl;

                instancing.withInstance([&](auto instance) {

                    uint32_t numAudioIn = instance->audioInputBuses().size();
                    uint32_t numAudioOut = instance->audioOutputBuses().size();
                    remidy::AudioProcessContext ctx{4096};
                    // FIXME: channel count is not precise.
                    for (int32_t i = 0, n = numAudioIn; i < n; ++i)
                        ctx.addAudioIn(2, 1024);
                    for (int32_t i = 0, n = numAudioOut; i < n; ++i)
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

                    auto code = instance->process(ctx);
                    if (code != remidy::StatusCode::OK)
                        std::cerr << "  " << format->name() << ": " << displayName << " : process() failed. Error code " << (int32_t) code << std::endl;
                    else
                        successful = true;

                });
            }
            if (successful)
                std::cerr << "  " << format->name() << ": Successfully instantiated and deleted " << displayName << std::endl;
        }
    }

    return 0;
}

public:
int run(int argc, const char* argv[]) {
    int result{0};
    CPPTRACE_TRY {
        remidy::EventLoop::initializeOnUIThread();
        result = scanner.performPluginScanning();
        scanner.savePluginListCache();
        std::cerr << "Scanning completed. Start testing instancing... " << std::endl;

        std::thread thread([&] {
            CPPTRACE_TRY {
                result = testInstancing();
                remidy::EventLoop::stop();

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
