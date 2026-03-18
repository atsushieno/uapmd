#include <filesystem>
#include <format>
#include "InProcessScanSessionManager.hpp"
#include "remidy-tooling/priv/PluginScanTool.hpp"

namespace remidy_tooling {

int InProcessScanSessionManager::runScan(PluginScanTool& tool,
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
        auto fileScanning = dynamic_cast<FileBasedPluginScanning*>(scanning);
        if (!fileScanning)
            continue;
        auto formatName = format->name();
        for (const auto& bundlePath : entry.bundles) {
            if (tool.isBundleBlocklisted(formatName, bundlePath))
                continue;
            if (tool.isScanCancellationRequested(observer)) {
                tool.notifyScanError("Scan canceled.", observer);
                return -1;
            }
            tool.notifyBundleScanStarted(bundlePath, observer);
            try {
                auto results = fileScanning->scanBundle(bundlePath, requireFastScanning, bundleTimeoutSeconds);
                tool.mergeScanResults(std::move(results));
            } catch (...) {
                tool.notifyBundleScanCompleted(bundlePath, observer);
                throw;
            }
            tool.notifyBundleScanCompleted(bundlePath, observer);
        }
    }
    std::filesystem::current_path(savedCwd);
    return 0;
}

} // namespace remidy_tooling
