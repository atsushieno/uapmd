#pragma once

#include <filesystem>
#include <vector>

namespace remidy {
    class PluginFormat;
}

namespace uapmd_plugin_hosting {
    class PluginScanTool;
    struct PluginScanObserver;

    struct SlowScanEntry {
        remidy::PluginFormat* format{nullptr};
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
