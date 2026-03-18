#pragma once

#include "ScanSessionManager.hpp"

namespace remidy_tooling {
    class InProcessScanSessionManager final : public ScanSessionManager {
    public:
        int runScan(PluginScanTool& tool,
                    const SlowScanCatalog& catalog,
                    bool requireFastScanning,
                    std::filesystem::path& pluginListCacheFile,
                    bool forceRescan,
                    double bundleTimeoutSeconds,
                    PluginScanObserver* observer) override;
    };
}
