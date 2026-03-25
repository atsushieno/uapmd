#include <filesystem>
#include <format>
#include <mutex>
#include <condition_variable>
#include "InProcessScanSessionManager.hpp"
#include "remidy-tooling/priv/PluginScanTool.hpp"

namespace remidy_tooling {

void InProcessScanSessionManager::runScan(PluginScanTool& tool,
                                          const SlowScanCatalog& catalogPlan,
                                          bool requireFastScanning,
                                          std::filesystem::path&,
                                          bool,
                                          double bundleTimeoutSeconds,
                                          PluginScanObserver* observer) {
    auto savedCwd = std::filesystem::current_path();
    for (const auto& entry : catalogPlan) {
        auto* format = entry.format;
        if (!format)
            continue;
        auto scanning = format->scanning();
        if (!scanning)
            continue;
        auto fileScanning = dynamic_cast<FileOrUrlBasedPluginScanning*>(scanning);
        if (!fileScanning)
            continue;
        auto formatName = format->name();
        for (const auto& bundlePath : entry.bundles) {
            if (tool.isBundleBlocklisted(formatName, bundlePath))
                continue;
            if (tool.isScanCancellationRequested(observer)) {
                tool.notifyScanError("Scan canceled.", observer);
                std::filesystem::current_path(savedCwd);
                return;
            }
            tool.notifyBundleScanStarted(bundlePath, observer);
            std::mutex mutex;
            std::condition_variable condition;
            bool completed = false;
            std::string error;
            std::vector<PluginCatalogEntry> results;
            fileScanning->scanBundle(bundlePath, requireFastScanning, bundleTimeoutSeconds,
                                     [&](PluginCatalogEntry result) {
                                         std::lock_guard<std::mutex> lock(mutex);
                                         results.emplace_back(std::move(result));
                                     },
                                     [&](std::string scanError) {
                                         {
                                             std::lock_guard<std::mutex> lock(mutex);
                                             error = std::move(scanError);
                                             completed = true;
                                         }
                                         condition.notify_one();
                                     });
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [&] { return completed; });
            }
            if (!error.empty()) {
                tool.notifyBundleScanCompleted(bundlePath, observer);
                throw std::runtime_error(error);
            }
            tool.mergeScanResults(std::move(results));
            tool.notifyBundleScanCompleted(bundlePath, observer);
        }
    }
    std::filesystem::current_path(savedCwd);
}

} // namespace remidy_tooling
