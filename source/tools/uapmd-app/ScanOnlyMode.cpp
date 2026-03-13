#include "ScanOnlyMode.hpp"

#include <cstdlib>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__EMSCRIPTEN__) || ANDROID || (defined(__APPLE__) && TARGET_OS_IPHONE)

namespace uapmd {

int runScanOnlyMode(const ScanOnlyOptions&) {
    return EXIT_FAILURE;
}

}

#else

#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include <choc/text/choc_JSON.h>
#include <remidy/remidy.hpp>
#include <remidy-tooling/PluginScanTool.hpp>
#include <remidy-tooling/PluginInstancing.hpp>

namespace uapmd {
namespace {

struct VerificationFailure {
    std::string format;
    std::string pluginId;
    std::string displayName;
    std::string message;
};

struct VerificationReport {
    size_t attempted = 0;
    size_t succeeded = 0;
    std::vector<VerificationFailure> failures;
};

VerificationReport runFullVerification(remidy_tooling::PluginScanTool& scanner) {
    VerificationReport report{};

    auto runInstancing = [&]() {
        remidy::audioThreadIds().push_back(std::this_thread::get_id());

        for (auto format : scanner.formats()) {
            auto plugins = scanner.filterByFormat(scanner.catalog.getPlugins(), format->name());
            size_t index = 0;
            for (auto info : plugins) {
                ++index;
                if (!scanner.safeToInstantiate(format, info)) {
                    continue;
                }

                ++report.attempted;
                bool successful = false;
                remidy_tooling::PluginInstancing instancing{scanner, format, info};
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
                    report.failures.push_back(VerificationFailure{
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
                        report.failures.push_back(VerificationFailure{
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
            report.failures.push_back(VerificationFailure{
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

choc::value::Value buildPluginArray(remidy_tooling::PluginScanTool& scanner) {
    auto catalogEntries = scanner.catalog.getPlugins();
    std::vector<choc::value::Value> plugins;
    plugins.reserve(catalogEntries.size());
    for (auto entry : catalogEntries) {
        plugins.emplace_back(choc::value::createObject(
            "PluginCatalogEntry",
            "format", std::string{entry->format()},
            "id", std::string{entry->pluginId()},
            "name", std::string{entry->displayName()},
            "vendor", std::string{entry->vendorName()},
            "url", std::string{entry->productUrl()},
            "bundle", std::string{entry->bundlePath().string()}
        ));
    }
    return choc::value::createArray(plugins);
}

}

int runScanOnlyMode(const ScanOnlyOptions& options) {
    remidy::EventLoop::initializeOnUIThread();

    remidy_tooling::PluginScanTool scanner{};
    static std::filesystem::path emptyPath{};

    int scanResult = options.forceRescan
                         ? scanner.performPluginScanning(false, emptyPath)
                         : scanner.performPluginScanning(false);

    choc::value::Value pluginArray = buildPluginArray(scanner);
    bool success = (scanResult == 0);

    if (success)
        scanner.savePluginListCache();

    choc::value::Value fullVerificationJson;
    if (options.fullVerification) {
        auto report = runFullVerification(scanner);
        bool verificationSuccess = report.failures.empty();
        success = success && verificationSuccess;

        std::vector<choc::value::Value> failureArray;
        failureArray.reserve(report.failures.size());
        for (const auto& failure : report.failures) {
            failureArray.emplace_back(choc::value::createObject(
                "Failure",
                "format", failure.format,
                "pluginId", failure.pluginId,
                "name", failure.displayName,
                "message", failure.message
            ));
        }

        fullVerificationJson = choc::value::createObject(
            "FullVerification",
            "enabled", true,
            "attempted", static_cast<int64_t>(report.attempted),
            "succeeded", static_cast<int64_t>(report.succeeded),
            "failed", static_cast<int64_t>(report.failures.size()),
            "failures", choc::value::createArray(failureArray)
        );
    } else {
        fullVerificationJson = choc::value::createObject(
            "FullVerification",
            "enabled", false,
            "attempted", 0,
            "succeeded", 0,
            "failed", 0,
            "failures", choc::value::createArray(std::vector<choc::value::Value>{})
        );
    }

    choc::value::Value root;
    if (success) {
        root = choc::value::createObject(
            "PluginScanResult",
            "success", true,
            "forceRescan", options.forceRescan,
            "cacheFile", std::string{scanner.pluginListCacheFile().string()},
            "plugins", pluginArray,
            "fullVerification", fullVerificationJson
        );
    } else {
        std::string error = (scanResult != 0)
                                ? std::format("Scanner exited with code {}", scanResult)
                                : "Full verification reported failures";
        root = choc::value::createObject(
            "PluginScanResult",
            "success", false,
            "forceRescan", options.forceRescan,
            "cacheFile", std::string{scanner.pluginListCacheFile().string()},
            "plugins", pluginArray,
            "fullVerification", fullVerificationJson,
            "error", error
        );
    }

    std::cout << choc::json::toString(root, true) << std::endl;

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

}

#endif
