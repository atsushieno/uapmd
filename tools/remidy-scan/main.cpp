#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#include <cpptrace/from_current.hpp>
#include <cpplocate/cpplocate.h>
#include <remidy/remidy.hpp>
#include <remidy-tooling/PluginScanning.hpp>
#include <remidy-tooling/PluginInstancing.hpp>

// -------- instancing --------
const char* TOOLING_DIR_NAME= "remidy-tooling";

remidy_tooling::PluginScanning scanner{};

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
                remidy_tooling::PluginInstancing instancing{scanner, format, info};
                // ...
                // you could adjust configuration here
                // ...
                instancing.makeAlive([&](std::string err) {
                });
                while (instancing.instancingState() == remidy_tooling::PluginInstancingState::Created ||
                       instancing.instancingState()  == remidy_tooling::PluginInstancingState::Preparing)
                    std::this_thread::yield();

                std::cerr << "  " << format->name() << ": Successfully configured " << displayName << ". Instantiating now..." << std::endl;

                instancing.withInstance([&](auto instance) {

                    uint32_t numAudioIn = instance->audioInputBuses().size();
                    uint32_t numAudioOut = instance->audioOutputBuses().size();
                    remidy::AudioProcessContext ctx{4096};
                    for (int32_t i = 0, n = numAudioIn; i < n; ++i)
                        ctx.addAudioIn(instance->audioInputBuses()[i]->channelLayout().channels(), 1024);
                    for (int32_t i = 0, n = numAudioOut; i < n; ++i)
                        ctx.addAudioOut(instance->audioOutputBuses()[i]->channelLayout().channels(), 1024);
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
    int result{EXIT_SUCCESS};

    bool help = false;
    bool rescan = false;
    bool performInstantVerification = false;
    for (auto i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") || !strcmp(argv[i], "-?"))
            help = true;
        if (!strcmp(argv[i], "-rescan") || !strcmp(argv[i], "--rescan"))
            rescan = true;
        if (!strcmp(argv[i], "-full") || !strcmp(argv[i], "--full"))
            performInstantVerification = true;
    }
    if (help) {
        std::cerr << "Usage: " << argv[0] << "[-?|-help] [-rescan] [-full]" << std::endl;
        std::cerr << "  -?: show this help message." << std::endl;
        std::cerr << "  -rescan: perform rescanning and update plugin list cache." << std::endl;
        std::cerr << "  -full: perform full scanning with instant verification (actually create instance)." << std::endl;
        return EXIT_SUCCESS;
    }

    remidy::EventLoop::initializeOnUIThread();

    static std::filesystem::path emptyPath{};

    auto dir = cpplocate::localDir(TOOLING_DIR_NAME);
    auto pluginListCacheFile = dir.empty() ?  std::filesystem::path{""} : std::filesystem::path{dir}.append("plugin-list-cache.json");

    if (performInstantVerification)
        std::cerr << "Full scanning, ignoring existing plugin list cache..." << std::endl;
    else
        std::cerr << "Trying to load plugin list cache from " << pluginListCacheFile << std::endl;

    result = scanner.performPluginScanning(performInstantVerification ? emptyPath : pluginListCacheFile);

    scanner.savePluginListCache(pluginListCacheFile);
    std::cerr << "Scanning completed and saved plugin list cache: " << pluginListCacheFile << std::endl;

    if (!performInstantVerification) {
        std::cerr << "To perform full instanti scanning, pass `-full` argument." << std::endl;
        return EXIT_SUCCESS;
    }

    std::cerr << "Start testing instantiation... " << std::endl;

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

    return result;
}

};

int main(int argc, const char* argv[]) {
    CPPTRACE_TRY {
        RemidyScan scanner{};
        return scanner.run(argc, argv);
    } CPPTRACE_CATCH(const std::exception& e) {
        std::cerr << "Exception in testCreateInstance: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        return EXIT_FAILURE;
    }
}
