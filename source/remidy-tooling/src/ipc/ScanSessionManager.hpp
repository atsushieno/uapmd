#pragma once

#include <filesystem>
#include <vector>

namespace remidy {
    class PluginFormat;
}

namespace remidy_tooling {
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

        virtual int runScan(PluginScanTool& tool,
                            const SlowScanCatalog& catalog,
                            bool requireFastScanning,
                            std::filesystem::path& pluginListCacheFile,
                            bool forceRescan,
                            double bundleTimeoutSeconds,
                            PluginScanObserver* observer) = 0;
    };
}
