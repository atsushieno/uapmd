#pragma once

#include "ScanSessionManager.hpp"

namespace uapmd_plugin_hosting {
    class InProcessScanSessionManager final : public ScanSessionManager {
    public:
        void runScan(PluginScanTool& tool,
                     const SlowScanCatalog& catalog,
                     bool requireFastScanning,
                     std::filesystem::path& pluginListCacheFile,
                     bool forceRescan,
                     double bundleTimeoutSeconds,
                     PluginScanObserver* observer) override;
    };
}
