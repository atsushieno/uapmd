#include "../../include/uapmd-plugin-hosting/detail/scanner/ScanOnlyRunner.hpp"

#include <cstdlib>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__EMSCRIPTEN__) || ANDROID || (defined(__APPLE__) && TARGET_OS_IPHONE)

namespace uapmd_plugin_hosting {

int runScanOnlyMode(const uapmd_plugin_hosting::ScanOnlyOptions&, uapmd_plugin_hosting::ScanOnlyReport*) {
    return EXIT_FAILURE;
}

}

#else

#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>

#include <remidy/remidy.hpp>
#include <uapmd-plugin-hosting/uapmd-plugin-hosting.hpp>

namespace uapmd_plugin_hosting {
namespace {

ScanVerificationReport runFullVerification(uapmd_plugin_hosting::PluginScanTool& scanner) {
    ScanVerificationReport report{};
    report.enabled = true;

    auto runInstancing = [&]() {
        remidy::audioThreadIds().push_back(std::this_thread::get_id());

        for (auto format : scanner.formats()) {
            auto plugins = scanner.filterByFormat(scanner.catalog().getPlugins(), format->name());
            size_t index = 0;
            for (auto info : plugins) {
                ++index;
                if (!scanner.safeToInstantiate(format, info)) {
                    continue;
                }

                ++report.attempted;
                bool successful = false;
                uapmd_plugin_hosting::PluginInstancing instancing{scanner, format, info};
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
                if (!errorCopy.empty() || state != uapmd_plugin_hosting::PluginInstancingState::Ready) {
                    if (errorCopy.empty())
                        errorCopy = "Plugin did not reach ready state.";
                    report.failures.push_back(ScanVerificationFailure{
                        format->name(),
                        info->pluginId(),
                        info->displayName(),
                        errorCopy
                    });
                    continue;
                }

                instancing.withInstance([&](auto instance) {
                    const auto& inputBuses = instance->audioBuses()->audioInputBuses();
                    const auto& outputBuses = instance->audioBuses()->audioOutputBuses();

                    size_t numAudioIn = inputBuses.size();
                    size_t numAudioOut = outputBuses.size();
                    remidy::MasterContext masterContext;
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
                        for (size_t ch = 0, nCh = ctx.outputChannelCount(i); ch < nCh; ch++)
                            memcpy(ctx.getFloatOutBuffer(i, ch), (void*) "02468ACE13579BDF", 16);
                    }

                    auto code = instance->process(ctx);
                    if (code == remidy::StatusCode::OK)
                        successful = true;
                    else {
                        report.failures.push_back(ScanVerificationFailure{
                            format->name(),
                            info->pluginId(),
                            info->displayName(),
                            std::format("process() failed with status {}", static_cast<int32_t>(code))
                        });
                    }

                });

                if (successful)
                    ++report.succeeded;
            }
        }

        remidy::EventLoop::stop();
    };

    std::thread worker([&]() {
        try {
            runInstancing();
        } catch (const std::exception& e) {
            report.failures.push_back(ScanVerificationFailure{
                "*",
                "*",
                "",
                std::string{"Exception during verification: "} + e.what()
            });
            remidy::EventLoop::stop();
        }
    });
    remidy::EventLoop::start();
    if (worker.joinable())
        worker.join();
    return report;
}

std::vector<ScannedPluginEntry> buildPluginList(uapmd_plugin_hosting::PluginScanTool& scanner) {
    auto catalogEntries = scanner.catalog().getPlugins();
    std::vector<ScannedPluginEntry> plugins;
    plugins.reserve(catalogEntries.size());
    for (auto entry : catalogEntries) {
        plugins.push_back(ScannedPluginEntry{
            std::string{entry->format()},
            std::string{entry->pluginId()},
            std::string{entry->displayName()},
            std::string{entry->vendorName()},
            std::string{entry->productUrl()},
            entry->bundlePath().string()
        });
    }
    return plugins;
}

}

int runScanOnlyMode(const ScanOnlyOptions& options, ScanOnlyReport* outReport) {
    remidy::EventLoop::initializeOnUIThread();

    auto scanner = uapmd_plugin_hosting::PluginScanTool::create();
    auto scanMode = options.useRemoteScanner
        ? uapmd_plugin_hosting::ScanMode::Remote
        : uapmd_plugin_hosting::ScanMode::InProcess;

    std::mutex scanMutex;
    std::condition_variable scanCondition;
    bool scanCompleted = false;
    bool scanFailed = false;
    std::string scanError;
    uapmd_plugin_hosting::PluginScanObserver observer;
    observer.errorOccurred = [&](const std::string& message) {
        std::lock_guard<std::mutex> lock(scanMutex);
        scanFailed = true;
        scanError = message;
    };
    observer.slowScanCompleted = [&]() {
        {
            std::lock_guard<std::mutex> lock(scanMutex);
            scanCompleted = true;
        }
        scanCondition.notify_one();
    };

    scanner->performPluginScanning(false,
                                   scanMode,
                                   options.forceRescan,
                                   options.bundleTimeoutSeconds,
                                   &observer);
    {
        std::unique_lock<std::mutex> lock(scanMutex);
        scanCondition.wait(lock, [&] { return scanCompleted; });
    }

    if (!scanner->pluginListCacheFile().empty()) {
        try {
            scanner->savePluginListCache();
        } catch (const std::exception& e) {
            std::cerr << "Failed to save plugin list cache: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Failed to save plugin list cache: unknown error" << std::endl;
        }
    }
    scanner->flushBlocklist();

    ScanOnlyReport report{};
    report.forceRescan = options.forceRescan;
    report.cacheFile = scanner->pluginListCacheFile();
    report.plugins = buildPluginList(*scanner);

    bool success = !scanFailed;

    if (options.fullVerification) {
        report.fullVerification = runFullVerification(*scanner);
        success = success && report.fullVerification.failures.empty();
    } else {
        report.fullVerification = ScanVerificationReport{};
    }

    report.success = success;
    if (!success)
        report.error = !scanError.empty() ? scanError : "Full verification reported failures";

    if (outReport)
        *outReport = std::move(report);

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

}

#endif
