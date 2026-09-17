#pragma once

#include <filesystem>
#include <vector>

namespace uapmd_plugin_hosting {
    class AudioPluginFormat;
    class PluginScanTool;
    struct PluginScanObserver;

    struct SlowScanEntry {
        AudioPluginFormat* format{nullptr};
        std::vector<std::filesystem::path> bundles;
    };

    using SlowScanCatalog = std::vector<SlowScanEntry>;

    class ScanSessionManager {
    public:
        virtual ~ScanSessionManager() = default;

        virtual void runScan(PluginScanTool& tool,
                             const SlowScanCatalog& catalog,
                             bool requireFastScanning,
                             std::filesystem::path& pluginListCacheFile,
                             bool forceRescan,
                             double bundleTimeoutSeconds,
                             PluginScanObserver* observer) = 0;
    };
}
