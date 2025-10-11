#include <thread>
#include <cstring>
#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>
#include <mutex>

#include <cpptrace/from_current.hpp>
#include <remidy/remidy.hpp>
#include <remidy-tooling/PluginScanTool.hpp>
#include <remidy-tooling/PluginInstancing.hpp>

// -------- instancing --------

remidy_tooling::PluginScanTool scanner{};

class RemidyScan {

int testInstancing() {
    for (auto format : scanner.formats()) {
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
                std::atomic<bool> instantiationFinished{false};
                std::mutex errorMutex;
                std::string instantiationError;
                instancing.makeAlive([&](std::string err) {
                    {
                        std::lock_guard<std::mutex> lock(errorMutex);
                        instantiationError = std::move(err);
                    }
                    instantiationFinished.store(true);
                    instantiationFinished.notify_one();
                });
                instantiationFinished.wait(false);

                auto state = instancing.instancingState().load();
                std::string errorCopy;
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    errorCopy = instantiationError;
                }
                if (!errorCopy.empty() || state != remidy_tooling::PluginInstancingState::Ready) {
                    if (errorCopy.empty())
                        errorCopy = "Plugin did not reach ready state.";
                    std::cerr << "  " << format->name() << ": " << displayName << " : instantiation failed. " << errorCopy << std::endl;
                    continue;
                }

                std::cerr << "  " << format->name() << ": Successfully configured " << displayName << ". Instantiating now..." << std::endl;

                instancing.withInstance([&](auto instance) {
                    const auto& inputBuses = instance->audioBuses()->audioInputBuses();
                    const auto& outputBuses = instance->audioBuses()->audioOutputBuses();

                    size_t numAudioIn = inputBuses.size();
                    size_t numAudioOut = outputBuses.size();
                    remidy::MasterContext masterContext;
                    remidy::TrackContext trackContext{masterContext};
                    remidy::AudioProcessContext ctx{masterContext, 4096};
                    constexpr size_t bufferCapacityFrames = 1024;
                    auto audioBuses = instance->audioBuses();
                    int32_t mainInIndex = audioBuses->mainInputBusIndex();
                    int32_t mainOutIndex = audioBuses->mainOutputBusIndex();
                    if (mainInIndex < 0 && numAudioIn > 0)
                        mainInIndex = 0;
                    if (mainOutIndex < 0 && numAudioOut > 0)
                        mainOutIndex = 0;

                    auto mainInChannels = (mainInIndex >= 0 && static_cast<size_t>(mainInIndex) < numAudioIn)
                                              ? inputBuses[static_cast<size_t>(mainInIndex)]->channelLayout().channels()
                                              : 0;
                    auto mainOutChannels = (mainOutIndex >= 0 && static_cast<size_t>(mainOutIndex) < numAudioOut)
                                               ? outputBuses[static_cast<size_t>(mainOutIndex)]->channelLayout().channels()
                                               : 0;

                    ctx.configureMainBus(static_cast<int32_t>(mainInChannels),
                                         static_cast<int32_t>(mainOutChannels),
                                         bufferCapacityFrames);

                    for (size_t i = 0; i < numAudioIn; ++i) {
                        if (static_cast<int32_t>(i) == mainInIndex)
                            continue;
                        ctx.addAudioIn(static_cast<int32_t>(inputBuses[i]->channelLayout().channels()),
                                       bufferCapacityFrames);
                    }
                    for (size_t i = 0; i < numAudioOut; ++i) {
                        if (static_cast<int32_t>(i) == mainOutIndex)
                            continue;
                        ctx.addAudioOut(static_cast<int32_t>(outputBuses[i]->channelLayout().channels()),
                                        bufferCapacityFrames);
                    }

                    ctx.frameCount(512);
                    for (size_t i = 0; i < ctx.audioInBusCount(); i++) {
                        for (size_t ch = 0, nCh = ctx.inputChannelCount(i); ch < nCh; ch++)
                            memcpy(ctx.getFloatInBuffer(i, ch), (void*) "0123456789ABCDEF", 16);
                    }
                    for (size_t i = 0; i < ctx.audioOutBusCount(); i++) {
                        // should not matter for audio processing though
                        for (size_t ch = 0, nCh = ctx.outputChannelCount(i); ch < nCh; ch++)
                            memcpy(ctx.getFloatOutBuffer(i, ch), (void*) "02468ACE13579BDF", 16);
                    }

                    auto code = instance->process(ctx);
                    if (code != remidy::StatusCode::OK)
                        std::cerr << "  " << format->name() << ": " << displayName << " : process() failed. Error code " << (int32_t) code << std::endl;
                    else
                        successful = true;

                });
                std::cerr << "  " << format->name() << ": " << displayName << " : process() completed." << std::endl;
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

    if (rescan)
        std::cerr << "Full scanning, ignoring existing plugin list cache..." << std::endl;
    else
        std::cerr << "Trying to load plugin list cache from " << scanner.pluginListCacheFile() << std::endl;

    if (rescan)
        result = scanner.performPluginScanning(emptyPath);
    else
        result = scanner.performPluginScanning();

    scanner.savePluginListCache();
    std::cerr << "Scanning completed and saved plugin list cache: " << scanner.pluginListCacheFile() << std::endl;

    if (!performInstantVerification) {
        std::cerr << "To perform full instance scanning, pass `-full` argument." << std::endl;
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
