#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "remidy/remidy.hpp"

namespace remidy_tooling {
    using namespace remidy;

    enum class ScanMode {
        InProcess,
        Remote
    };
    struct PluginScanObserver {
        std::function<void(uint32_t totalBundles)> slowScanStarted{};
        std::function<void(const std::filesystem::path& bundlePath)> bundleScanStarted{};
        std::function<void(const std::filesystem::path& bundlePath)> bundleScanCompleted{};
        std::function<void(bool success)> slowScanCompleted{};
        std::function<void(const std::string& message)> errorOccurred{};
        std::function<bool()> shouldCancel{};
    };

    struct BlocklistEntry {
        std::string id;
        std::string format;
        std::string pluginId;
        std::string reason;
        std::chrono::system_clock::time_point timestamp;
    };

    class PluginScanTool {
    public:
        virtual ~PluginScanTool() = default;
        static std::unique_ptr<PluginScanTool> create();

        virtual PluginCatalog& catalog() = 0;
        virtual const PluginCatalog& catalog() const = 0;

        virtual std::vector<PluginCatalogEntry*> filterByFormat(std::vector<PluginCatalogEntry*> entries,
                                                                std::string format) = 0;

        virtual std::vector<PluginFormat*> formats() = 0;
        virtual void addFormat(PluginFormat* item) = 0;

        virtual std::filesystem::path& pluginListCacheFile() = 0;
        virtual int performPluginScanning(bool requireFastScanning,
                                          ScanMode mode = ScanMode::InProcess,
                                          bool forceRescan = false,
                                          double bundleTimeoutSeconds = 0.0,
                                          PluginScanObserver* observer = nullptr) = 0;
        virtual int performPluginScanning(bool requireFastScanning,
                                          std::filesystem::path& pluginListCacheFile,
                                          ScanMode mode = ScanMode::InProcess,
                                          bool forceRescan = false,
                                          double bundleTimeoutSeconds = 0.0,
                                          PluginScanObserver* observer = nullptr) = 0;
        virtual void savePluginListCache() = 0;
        virtual void savePluginListCache(std::filesystem::path& fileToSave) = 0;
        virtual void flushBlocklist() = 0;

        virtual std::vector<BlocklistEntry> blocklistEntries() const = 0;
        virtual bool unblockBundle(const std::string& entryId) = 0;
        virtual void clearBlocklist() = 0;
        virtual void addToBlocklist(const std::string& formatName, const std::string& pluginId, const std::string& reason) = 0;
        virtual std::string lastScanError() const = 0;

        virtual bool safeToInstantiate(PluginFormat* format, PluginCatalogEntry* entry) = 0;
        virtual bool shouldCreateInstanceOnUIThread(PluginFormat* format, PluginCatalogEntry* entry) = 0;
        virtual bool isBundleBlocklisted(const std::string& formatName, const std::filesystem::path& bundlePath) const = 0;

        friend class InProcessScanSessionManager;
        friend class RemoteScanSessionManager;

    protected:
        virtual void mergeScanResults(std::vector<std::unique_ptr<PluginCatalogEntry>> results) = 0;
        virtual void notifyBundleScanStarted(const std::filesystem::path& bundlePath,
                                             PluginScanObserver* observer) const = 0;
        virtual void notifyBundleScanCompleted(const std::filesystem::path& bundlePath,
                                               PluginScanObserver* observer) const = 0;
        virtual void notifySlowScanStarted(uint32_t totalBundles, PluginScanObserver* observer) const = 0;
        virtual void notifySlowScanCompleted(bool success, PluginScanObserver* observer) const = 0;
        virtual void notifyScanError(const std::string& message, PluginScanObserver* observer) = 0;
        virtual bool isScanCancellationRequested(PluginScanObserver* observer) const = 0;
    };
}
