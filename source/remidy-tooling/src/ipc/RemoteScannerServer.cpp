#include "remidy-tooling/detail/RemoteScannerServer.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <choc/text/choc_JSON.h>

#include "IpcJsonChannel.hpp"
#include "ScannerProtocol.hpp"
#include "TcpSocket.hpp"
#include "ScanSessionManager.hpp"
#include "remidy-tooling/detail/PluginScanTool.hpp"

namespace remidy_tooling {

int runRemoteScannerServer(const RemoteScannerServerOptions& options) {
#if ANDROID || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    (void) options;
    return EXIT_FAILURE;
#else
    if (options.host.empty() || options.port == 0 || options.token.empty()) {
        std::cerr << "Invalid IPC parameters for remote scanner server." << std::endl;
        return EXIT_FAILURE;
    }
    if (!ipc::ensureSocketLayerInitialized()) {
        std::cerr << "Failed to initialize socket stack." << std::endl;
        return EXIT_FAILURE;
    }

    ipc::TcpSocket socket;
    if (!socket.connect(options.host, options.port)) {
        std::cerr << "Unable to connect to IPC server at " << options.host << ":" << options.port << std::endl;
        return EXIT_FAILURE;
    }

    ipc::IpcJsonChannel channel(std::move(socket));
    choc::value::Value helloPayload = choc::value::createObject("Hello");
    helloPayload.setMember("token", options.token);
    ipc::IpcMessage hello{
        .type = ipc::kScannerMsgHello,
        .requestId = "hello",
        .payload = helloPayload
    };
    if (!channel.send(hello)) {
        std::cerr << "Failed to send hello message to IPC server." << std::endl;
        return EXIT_FAILURE;
    }

    auto startMessage = channel.receive(10000);
    if (!startMessage.has_value() || startMessage->type != ipc::kScannerMsgStartScan) {
        std::cerr << "Did not receive start command from IPC server." << std::endl;
        return EXIT_FAILURE;
    }

    auto forceVal = startMessage->payload["forceRescan"];
    auto fastVal = startMessage->payload["requireFastScanning"];
    bool forceRescan = !forceVal.isVoid() && forceVal.getBool();
    bool requireFast = !fastVal.isVoid() && fastVal.getBool();
    auto cacheValue = startMessage->payload["cacheFile"];

    remidy::EventLoop::initializeOnUIThread();
    auto scanner = PluginScanTool::create();
    if (!cacheValue.isVoid())
        scanner->pluginListCacheFile() = std::filesystem::path(cacheValue.toString());
    if (!scanner->pluginListCacheFile().empty()) {
        std::error_code ec;
        if (std::filesystem::exists(scanner->pluginListCacheFile(), ec))
            scanner->catalog().load(scanner->pluginListCacheFile());
    }

    auto planValue = startMessage->payload["slowCatalog"];
    auto totalValue = startMessage->payload["totalBundles"];
    auto timeoutValue = startMessage->payload["timeoutSeconds"];
    double timeoutSeconds = 0.0;
    if (!timeoutValue.isVoid()) {
        if (timeoutValue.isFloat())
            timeoutSeconds = timeoutValue.getFloat64();
        else if (timeoutValue.isInt())
            timeoutSeconds = static_cast<double>(timeoutValue.getInt64());
    }
    if (timeoutSeconds < 0.0)
        timeoutSeconds = 0.0;

    SlowScanCatalog slowCatalog;
    slowCatalog.reserve(planValue.isArray() ? planValue.size() : 0);

    std::unordered_map<std::string, remidy::PluginFormat*> formatLookup;
    for (auto* format : scanner->formats()) {
        if (format)
            formatLookup.emplace(format->name(), format);
    }
    if (!planValue.isVoid() && planValue.isArray()) {
        for (auto entry : planValue) {
            auto fmtVal = entry["format"];
            auto bundlesVal = entry["bundles"];
            if (fmtVal.isVoid() || !bundlesVal.isArray())
                continue;
            auto it = formatLookup.find(fmtVal.toString());
            if (it == formatLookup.end())
                continue;
            SlowScanEntry planEntry;
            planEntry.format = it->second;
            for (auto bundleVal : bundlesVal) {
                if (!bundleVal.isVoid())
                    planEntry.bundles.emplace_back(bundleVal.toString());
            }
            if (!planEntry.bundles.empty())
                slowCatalog.push_back(std::move(planEntry));
        }
    }

    size_t totalBundles = 0;
    if (!totalValue.isVoid()) {
        if (totalValue.isInt())
            totalBundles = static_cast<size_t>(std::max<int64_t>(0, totalValue.getInt64()));
        else if (totalValue.isFloat())
            totalBundles = static_cast<size_t>(std::max<double>(0.0, totalValue.getFloat64()));
    }
    if (totalBundles == 0) {
        for (const auto& entry : slowCatalog)
            totalBundles += entry.bundles.size();
    }

    auto sendBundleStarted = [&](const std::string& format, const std::filesystem::path& bundlePath) {
        choc::value::Value payload = choc::value::createObject("BundleStarted");
        payload.setMember("format", format);
        payload.setMember("bundlePath", bundlePath.string());
        ipc::IpcMessage msg{
            .type = ipc::kScannerMsgBundleStarted,
            .requestId = {},
            .payload = payload
        };
        channel.send(msg);
    };
    auto sendBundleFinished = [&](const std::string& format, const std::filesystem::path& bundlePath) {
        choc::value::Value payload = choc::value::createObject("BundleFinished");
        payload.setMember("format", format);
        payload.setMember("bundlePath", bundlePath.string());
        ipc::IpcMessage msg{
            .type = ipc::kScannerMsgBundleFinished,
            .requestId = {},
            .payload = payload
        };
        channel.send(msg);
    };
    auto sendTotals = [&](uint32_t total, bool final) {
        choc::value::Value payload = choc::value::createObject("BundleTotals");
        payload.setMember("total", static_cast<int32_t>(total));
        payload.setMember("final", final);
        ipc::IpcMessage msg{
            .type = ipc::kScannerMsgBundleTotals,
            .requestId = {},
            .payload = payload
        };
        channel.send(msg);
    };
    sendTotals(static_cast<uint32_t>(totalBundles), true);

    auto checkCancellation = [&](std::string& cancellationError) {
        bool timedOut = false;
        auto message = channel.receive(0, timedOut);
        if (timedOut)
            return false;
        if (!message.has_value()) {
            cancellationError = "IPC connection closed.";
            return true;
        }
        if (message->type == ipc::kScannerMsgCancelScan)
            return true;
        return false;
    };

    bool success = true;
    std::string errorMessage;
    uint32_t processedBundles = 0;
    std::string lastBundleLabel;
    bool canceled = false;
    std::optional<std::filesystem::path> failedBundlePath;
    std::string failedBundleFormat;
    std::string failedBundleReason;

    auto describeBundle = [](const std::string& format, const std::filesystem::path& bundlePath) {
        auto filename = bundlePath.filename().string();
        if (filename.empty())
            filename = bundlePath.string();
        if (filename.empty())
            return format;
        return std::format("{}: {}", format, filename);
    };

    auto persistCatalog = [&]() {
        if (scanner->pluginListCacheFile().empty())
            return;
        try {
            scanner->savePluginListCache();
        } catch (const std::exception& e) {
            std::cerr << "Failed to save plugin list cache: " << e.what() << std::endl;
        }
    };

    for (const auto& entry : slowCatalog) {
        if (!entry.format)
            continue;
        auto scanning = entry.format->scanning();
        auto fileScanning = dynamic_cast<FileOrUrlBasedPluginScanning*>(scanning);
        if (!fileScanning)
            continue;
        auto formatName = entry.format->name();
        for (const auto& bundlePath : entry.bundles) {
            if (!canceled && checkCancellation(errorMessage)) {
                canceled = true;
                success = false;
                break;
            }
            sendBundleStarted(formatName, bundlePath);
            lastBundleLabel = describeBundle(formatName, bundlePath);
            std::mutex bundleMutex;
            std::condition_variable bundleCondition;
            bool bundleCompleted = false;
            std::string bundleError;
            std::vector<PluginCatalogEntry> bundleResults;
            fileScanning->scanBundle(bundlePath, requireFast, timeoutSeconds,
                                     [&](PluginCatalogEntry plugin) {
                                         std::lock_guard<std::mutex> lock(bundleMutex);
                                         bundleResults.emplace_back(std::move(plugin));
                                     },
                                     [&](std::string error) {
                                         {
                                             std::lock_guard<std::mutex> lock(bundleMutex);
                                             bundleError = std::move(error);
                                             bundleCompleted = true;
                                         }
                                         bundleCondition.notify_one();
                                     });
            {
                std::unique_lock<std::mutex> lock(bundleMutex);
                bundleCondition.wait(lock, [&] { return bundleCompleted; });
            }
            if (!bundleError.empty()) {
                success = false;
                errorMessage = bundleError;
                failedBundlePath = bundlePath;
                failedBundleFormat = formatName;
                failedBundleReason = errorMessage;
                sendBundleFinished(formatName, bundlePath);
                break;
            }
            for (auto& plugin : bundleResults) {
                if (!scanner->catalog().contains(plugin.format(), plugin.pluginId()))
                    scanner->catalog().add(std::move(plugin));
            }
            ++processedBundles;
            persistCatalog();
            sendBundleFinished(formatName, bundlePath);
        }
        if (!success || canceled)
            break;
    }

    if (canceled && errorMessage.empty())
        errorMessage = "Scan canceled.";

    persistCatalog();

    choc::value::Value resultPayload = choc::value::createObject("ScanResult");
    resultPayload.setMember("success", success);
    resultPayload.setMember("processedBundles", static_cast<int32_t>(processedBundles));
    resultPayload.setMember("totalBundles", static_cast<int32_t>(totalBundles));
    resultPayload.setMember("totalsFinal", true);
    resultPayload.setMember("running", false);
    if (!lastBundleLabel.empty())
        resultPayload.setMember("currentBundle", lastBundleLabel);
    if (!scanner->pluginListCacheFile().empty())
        resultPayload.setMember("cacheFile", scanner->pluginListCacheFile().string());
    if (!success) {
        auto err = scanner->lastScanError();
        if (err.empty())
            err = errorMessage.empty() ? "Remote scanning failed." : errorMessage;
        resultPayload.setMember("error", err);
        if (failedBundlePath.has_value()) {
            resultPayload.setMember("failedBundlePath", failedBundlePath->string());
            resultPayload.setMember("failedBundleFormat", failedBundleFormat);
            resultPayload.setMember("failedBundleReason", failedBundleReason.empty() ? err : failedBundleReason);
        }
    }
    ipc::IpcMessage result{
        .type = ipc::kScannerMsgScanResult,
        .requestId = "scanResult",
        .payload = resultPayload
    };
    channel.send(result);

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

} // namespace remidy_tooling
