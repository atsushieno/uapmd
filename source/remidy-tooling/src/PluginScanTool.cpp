
#include <algorithm>
#include <fstream>
#include <format>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <cstring>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// cpplocate is a desktop-only library: excluded on Android, Emscripten, and iOS.
// CMake sets UAPMD_ENABLE_CPPLOCATE=OFF for those platforms; guard the include here too.
#if !ANDROID && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#include <cpplocate/cpplocate.h>
#include <choc/platform/choc_Execute.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <choc/text/choc_JSON.h>
#include "remidy-tooling/remidy-tooling.hpp"
#include "remidy-tooling/priv/PluginFormatManager.hpp"
#include "ScanConstants.hpp"
#include "ipc/ScanSessionManager.hpp"
#include "ipc/InProcessScanSessionManager.hpp"
#include "ipc/RemoteScanSessionManager.hpp"

namespace remidy_tooling {

#ifdef __EMSCRIPTEN__
EM_JS(void, uapmd_sync_browser_fs_async, (), {
    if (typeof FS === 'undefined' || !FS.syncfs)
        return;
    FS.syncfs(false, function(err) {
        if (err)
            console.error('[uapmd] Failed to persist browser filesystem:', err);
    });
});
#endif

static void syncBrowserFsAsync() {
#ifdef __EMSCRIPTEN__
    uapmd_sync_browser_fs_async();
#endif
}

class PluginScanToolImpl final : public PluginScanTool {
public:
    PluginScanToolImpl();

    PluginCatalog& catalog() override { return catalog_; }
    const PluginCatalog& catalog() const override { return catalog_; }

    std::vector<PluginCatalogEntry*> filterByFormat(std::vector<PluginCatalogEntry*> entries,
                                                    std::string format) override {
        erase_if(entries, [format](PluginCatalogEntry* entry) { return entry->format() != format; });
        return entries;
    }

    std::vector<PluginFormat*> formats() override { return formatManager_.formats(); }
    void addFormat(PluginFormat* item) override { formatManager_.addFormat(item); }

    std::filesystem::path& pluginListCacheFile() override { return plugin_list_cache_file; }
    void performPluginScanning(bool requireFastScanning,
                               ScanMode mode,
                               bool forceRescan,
                               double bundleTimeoutSeconds,
                               PluginScanObserver* observer) override;
    void performPluginScanning(bool requireFastScanning,
                               std::filesystem::path& pluginListCacheFile,
                               ScanMode mode,
                               bool forceRescan,
                               double bundleTimeoutSeconds,
                               PluginScanObserver* observer) override;
    void savePluginListCache() override { savePluginListCache(plugin_list_cache_file); }
    void savePluginListCache(std::filesystem::path& fileToSave) override {
        catalog_.save(fileToSave);
        syncBrowserFsAsync();
    }
    void flushBlocklist() override { saveBlocklistToDisk(); }

    std::vector<BlocklistEntry> blocklistEntries() const override;
    bool unblockBundle(const std::string& entryId) override;
    void clearBlocklist() override;
    void addToBlocklist(const std::string& formatName, const std::string& pluginId, const std::string& reason) override;
    std::string lastScanError() const override;
    bool safeToInstantiate(PluginFormat* format, PluginCatalogEntry* entry) override;
    bool shouldCreateInstanceOnUIThread(PluginFormat* format, PluginCatalogEntry* entry) override;
    bool isBundleBlocklisted(const std::string& formatName, const std::filesystem::path& bundlePath) const override;

protected:
    void mergeScanResults(std::vector<PluginCatalogEntry> results) override;
    std::string makeBlocklistId(const std::string& formatName, const std::string& pluginId) const;
    bool isBlocklisted(const std::string& formatName, const std::string& pluginId) const;
    ScanSessionManager& ensureRemoteSessionManager();
    ScanSessionManager& ensureInProcessSessionManager();
    void loadBlocklistFromDisk();
    void saveBlocklistToDisk() const;
    bool canPersistBlocklist() const;
    void setLastScanError(std::string message);
    void notifyBundleScanStarted(const std::filesystem::path& bundlePath,
                                 PluginScanObserver* observer) const override;
    void notifyBundleScanCompleted(const std::filesystem::path& bundlePath,
                                   PluginScanObserver* observer) const override;
    void notifySlowScanStarted(uint32_t totalBundles, PluginScanObserver* observer) const override;
    void notifySlowScanCompleted(PluginScanObserver* observer) const override;
    void notifyScanError(const std::string& message, PluginScanObserver* observer) override;
    bool isScanCancellationRequested(PluginScanObserver* observer) const override;

private:
    SlowScanCatalog prepareSlowScanCatalog(const std::vector<PluginFormat*>& formats,
                                           bool requireFastScanning,
                                           std::filesystem::path& pluginListCacheFile,
                                           bool forceRescan,
                                           double bundleTimeoutSeconds,
                                           std::string& slowScanReportText);
    void executeSlowScanCatalog(const SlowScanCatalog& catalog,
                                const std::vector<PluginFormat*>& formats,
                                bool requireFastScanning,
                                std::filesystem::path& pluginListCacheFile,
                                ScanMode mode,
                                bool forceRescan,
                                double bundleTimeoutSeconds,
                                PluginScanObserver* observer);

    std::filesystem::path plugin_list_cache_file{};
    PluginFormatManager formatManager_{};
    PluginCatalog catalog_{};
    mutable std::mutex stateMutex_{};
    std::vector<BlocklistEntry> blocklistEntries_{};
    std::filesystem::path blocklist_file_{};
    std::unique_ptr<ScanSessionManager> inProcessSessionManager_{};
    std::unique_ptr<ScanSessionManager> remoteSessionManager_{};
    std::string lastScanErrorMessage_{};
};

std::optional<std::string> extractJsonPayload(const std::string& text) {
    auto locate = [](char openChar, char closeChar, size_t startIdx, const std::string& input) -> std::optional<std::string> {
        if (startIdx == std::string::npos)
            return std::nullopt;
        int depth = 0;
        bool inString = false;
        bool escape = false;
        for (size_t i = startIdx; i < input.size(); ++i) {
            char c = input[i];
            if (inString) {
                if (escape) {
                    escape = false;
                    continue;
                }
                if (c == '\\') {
                    escape = true;
                    continue;
                }
                if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"') {
                inString = true;
                continue;
            }
            if (c == openChar) {
                ++depth;
                continue;
            }
            if (c == closeChar) {
                --depth;
                if (depth == 0)
                    return input.substr(startIdx, i - startIdx + 1);
            }
        }
        return std::nullopt;
    };

    auto objectStart = text.find('{');
    auto arrayStart = text.find('[');
    bool useArray = false;
    size_t startIdx = std::string::npos;
    if (arrayStart != std::string::npos &&
        (objectStart == std::string::npos || arrayStart < objectStart)) {
        useArray = true;
        startIdx = arrayStart;
    } else {
        startIdx = objectStart;
    }

    if (useArray)
        return locate('[', ']', startIdx, text);
    return locate('{', '}', startIdx, text);
}

const char* TOOLING_DIR_NAME= "remidy-tooling";

PluginScanToolImpl::PluginScanToolImpl() {
#if ANDROID
    std::filesystem::path dir{};
#elif defined(__EMSCRIPTEN__)
    std::filesystem::path dir{"/browser/remidy-tooling"};
#elif (defined(__APPLE__) && TARGET_OS_IPHONE)
    // No filesystem-based plugin cache path on iOS (sandboxed app bundle).
    std::filesystem::path dir{};
#else
    auto dir = cpplocate::localDir(TOOLING_DIR_NAME);
#endif
    plugin_list_cache_file = dir.empty() ? std::filesystem::path{""} : std::filesystem::path{dir}.append(
            "plugin-list-cache.json");
    blocklist_file_ = dir.empty() ? std::filesystem::path{} : std::filesystem::path{dir}.append("plugin-blocklist.json");
    loadBlocklistFromDisk();

}

void PluginScanToolImpl::performPluginScanning(bool requireFastScanning,
                                               ScanMode mode,
                                               bool forceRescan,
                                               double bundleTimeoutSeconds,
                                               PluginScanObserver* observer) {
    performPluginScanning(requireFastScanning, plugin_list_cache_file, mode, forceRescan, bundleTimeoutSeconds, observer);
}

void PluginScanToolImpl::performPluginScanning(bool requireFastScanning,
                                               std::filesystem::path& pluginListCacheFile,
                                               ScanMode mode,
                                               bool forceRescan,
                                               double bundleTimeoutSeconds,
                                               PluginScanObserver* observer) {
    setLastScanError({});
    std::string planReport;
    const auto& formatList = formatManager_.formatView();
    auto catalogPlan = prepareSlowScanCatalog(formatList,
                                              requireFastScanning,
                                              pluginListCacheFile,
                                              forceRescan,
                                              bundleTimeoutSeconds,
                                              planReport);
    if (!planReport.empty())
        std::cout << planReport << std::endl;
    executeSlowScanCatalog(catalogPlan,
                           formatList,
                           requireFastScanning,
                           pluginListCacheFile,
                           mode,
                           forceRescan,
                           bundleTimeoutSeconds,
                           observer);
}

remidy_tooling::SlowScanCatalog PluginScanToolImpl::prepareSlowScanCatalog(const std::vector<PluginFormat*>& formats,
                                                                                       bool requireFastScanning,
                                                                                       std::filesystem::path& pluginListCacheFile,
                                                                                       bool forceRescan,
                                                                                       double bundleTimeoutSeconds,
                                                                                       std::string& slowScanReportText) {
    SlowScanCatalog catalogPlan;
    plugin_list_cache_file = pluginListCacheFile;
    slowScanReportText.clear();
    if (!forceRescan && !pluginListCacheFile.empty() && std::filesystem::exists(pluginListCacheFile)) {
        catalog_.load(pluginListCacheFile);
        slowScanReportText = "Slow scanning skipped (loaded plugin cache).\n";
        return catalogPlan;
    }

    if (forceRescan)
        catalog_.clear();

    size_t slowBundleTotal = 0;
    std::ostringstream planStream;
    planStream << "Slow scanning catalog\n";
    bool fastScanModified = false;

    for (auto* format : formats) {
        if (!format)
            continue;
        auto scanning = format->scanning();
        if (!scanning)
            continue;
        auto fileScanning = dynamic_cast<FileOrUrlBasedPluginScanning*>(scanning);

        bool scanMayBeSlow = scanning->scanningMayBeSlow();
        auto cachedEntries = filterByFormat(catalog_.getPlugins(), format->name());
        bool shouldScan = forceRescan || !scanMayBeSlow || cachedEntries.empty();
        if (!shouldScan)
            continue;

        auto fastResults = scanning->getAllFastScannablePlugins();
        if (!fastResults.empty()) {
            mergeScanResults(std::move(fastResults));
            fastScanModified = true;
        }

        if (requireFastScanning)
            continue;

        if (!scanMayBeSlow)
            continue;

        if (!fileScanning) {
            notifyScanError(std::format("Format {} reports slow scanning but does not implement FileOrUrlBasedPluginScanning.", format->name()), nullptr);
            continue;
        }

        auto bundles = fileScanning->enumerateCandidateBundles(requireFastScanning);
        std::vector<std::filesystem::path> slowBundles;
        slowBundles.reserve(bundles.size());
        for (const auto& bundle : bundles) {
            if (isBundleBlocklisted(format->name(), bundle))
                continue;
            bool requiresLoad = scanning->scanRequiresLoadLibrary(bundle);
            if (requiresLoad)
                slowBundles.push_back(bundle);
        }

        if (!slowBundles.empty()) {
            planStream << "\n[" << format->name() << "]\n";
            for (const auto& bundle : slowBundles) {
                planStream << " - " << bundle << "\n";
            }
            slowBundleTotal += slowBundles.size();
            catalogPlan.push_back(SlowScanEntry{format, std::move(slowBundles)});
        }
    }

    if (slowBundleTotal == 0)
        planStream << "\nAll plugin bundles were handled locally.\n";
    if (fastScanModified && !plugin_list_cache_file.empty()) {
        try {
            savePluginListCache(plugin_list_cache_file);
        } catch (...) {
            // ignore cache write errors here; slow scan will surface errors later if needed.
        }
    }
    slowScanReportText = planStream.str();
    return catalogPlan;
}

void PluginScanToolImpl::executeSlowScanCatalog(const SlowScanCatalog& catalogPlan,
                                                const std::vector<PluginFormat*>& /*formats*/,
                                                bool requireFastScanning,
                                                std::filesystem::path& pluginListCacheFile,
                                                ScanMode mode,
                                                bool forceRescan,
                                                double bundleTimeoutSeconds,
                                                PluginScanObserver* observer) {
    uint32_t totalBundles = 0;
    for (const auto& entry : catalogPlan)
        totalBundles += static_cast<uint32_t>(entry.bundles.size());
    if (totalBundles > 0)
        notifySlowScanStarted(totalBundles, observer);

    if (!catalogPlan.empty()) {
#if ANDROID || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        if (mode == ScanMode::Remote) {
            notifyScanError("Remote scanning is unavailable on this platform.", observer);
            notifySlowScanCompleted(observer);
            return;
        }
#endif
        if (mode == ScanMode::Remote && !pluginListCacheFile.empty())
            savePluginListCache(pluginListCacheFile);
        ScanSessionManager* manager = mode == ScanMode::Remote
                                      ? &ensureRemoteSessionManager()
                                      : &ensureInProcessSessionManager();
        manager->runScan(*this,
                         catalogPlan,
                         requireFastScanning,
                         pluginListCacheFile,
                         forceRescan,
                         bundleTimeoutSeconds,
                         observer);
    }

    notifySlowScanCompleted(observer);
}

void PluginScanToolImpl::mergeScanResults(std::vector<PluginCatalogEntry> results) {
    for (auto& entry : results) {
        if (!catalog_.contains(entry.format(), entry.pluginId()))
            catalog_.add(std::move(entry));
    }
}


bool PluginScanToolImpl::safeToInstantiate(PluginFormat* format, PluginCatalogEntry *entry) {
    auto displayName = entry->displayName();
    auto vendor = entry->vendorName();
    bool skip = false;

    if (isBlocklisted(format->name(), entry->pluginId()))
        return false;

    // Not sure when it started, but their AU version stalls while instantiating.
    // Note that they can be instantiated just fine. Maybe just instancing them among many.
    if (format->name() == "AU" && vendor == "Tracktion")
        skip = true;

    // They somehow trigger scanner crashes.
    // Note that they can be instantiated just fine. Maybe just instancing them among many.
    if (format->name() == "CLAP" && displayName == "Floe")
        skip = true;
    if (format->name() == "CLAP" && displayName == "Zebrify")
        skip = true;
    if (format->name() == "CLAP" && displayName == "Zebralette3")
        skip = true;
    if (format->name() == "CLAP" && displayName == "ysfx-s instrument")
        skip = true;
    if (format->name() == "CLAP" && displayName == "ysfx-s FX")
        skip = true;
    if (format->name() == "CLAP" && displayName == "Ragnarok2")
        skip = true;

    if (format->name() == "AU" && displayName == "RX 9 Monitor")
        skip = true;

#if __APPLE__ || WIN32
    // they generate *.dylib in .ttl, but the library is *.so...
    if (format->name() == "LV2" && displayName == "AIDA-X")
        skip = true;
    // they generate *.so in .ttl, but the library is *.dylib...
    if (format->name() == "LV2" && displayName.starts_with("sfizz"))
        skip = true;
#endif

    return !skip;
}

bool PluginScanToolImpl::shouldCreateInstanceOnUIThread(PluginFormat *format, PluginCatalogEntry* entry) {
    auto displayName = entry->displayName();
    auto vendor = entry->vendorName();
    bool forceMainThread =
        // not sure why
        format->name() == "AU" && displayName == "FL Studio"
        || format->name() == "AU" && displayName == "SINE Player"
        || format->name() == "AU" && displayName == "AUSpeechSynthesis"
        // QObject::killTimer: Timers cannot be stopped from another thread
        // QObject::~QObject: Timers cannot be stopped from another thread
        || format->name() == "AU" && displayName.contains("Massive X")
        // [threadmgrsupport] _TSGetMainThread_block_invoke():Main thread potentially initialized incorrectly, cf <rdar://problem/67741850>
        || format->name() == "AU" && displayName == "Bite"
        // WARNING: QApplication was not created in the main() thread
        || format->name() == "AU" && displayName == "Raum"
        || format->name() == "AU" && displayName == "Guitar Rig 6 FX"
        || format->name() == "AU" && displayName == "Guitar Rig 6 MFX"
        || format->name() == "AU" && displayName == "Vienna Synchron Player"
        // EXC_BAD_ACCESS, maybe because they are JUCE
        || format->name() == "AU" && displayName == "DDSP Effect"
        || format->name() == "AU" && displayName == "DDSP Synth"
        || format->name() == "AU" && displayName == "Plugin Buddy"
        || format->name() == "AU" && displayName == "Synthesizer V Studio Plugin"
        // EXC_BAD_ACCESS, not sure why
        || format->name() == "AU" && displayName == "BBC Symphony Orchestra"
        || format->name() == "AU" && displayName == "LABS"
        || format->name() == "AU" && displayName == "BFD Player"
        || format->name() == "AU" && displayName == "RX 9 Music Rebalance"
        || format->name() == "AU" && displayName == "RX 9 Spectral Editor"
        || format->name() == "AU" && displayName == "Ozone 9"
        || format->name() == "AU" && displayName == "Absynth 5"
        || format->name() == "AU" && displayName == "Absynth 5 MFX"
        || format->name() == "AU" && displayName == "FM8"
        || format->name() == "AU" && displayName == "Massive X"
        || format->name() == "AU" && displayName == "Reaktor 6"
        || format->name() == "AU" && displayName == "Reaktor 6 MFX"
        || format->name() == "AU" && displayName == "Reaktor 6 MIDIFX"
        || format->name() == "AU" && displayName == "Kontakt 8"
        // Some JUCE plugins are NOT designed to work in thread safe manner (JUCE behaves like VST3).
        // (Use AddressSanitizer to detect such failing ones.)
        //
        // (It may not be everything from Tracktion, but there are too many #T* plugins.)
        || format->name() == "AU" && vendor == "Tracktion"
    ;
    return forceMainThread || format->requiresUIThreadOn(entry) != PluginUIThreadRequirement::None;
}

std::vector<remidy_tooling::BlocklistEntry> PluginScanToolImpl::blocklistEntries() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return blocklistEntries_;
}

std::string PluginScanToolImpl::lastScanError() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return lastScanErrorMessage_;
}

bool PluginScanToolImpl::unblockBundle(const std::string& entryId) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = std::remove_if(blocklistEntries_.begin(), blocklistEntries_.end(), [&](const BlocklistEntry& entry) {
            return entry.id == entryId;
        });
        if (it != blocklistEntries_.end()) {
            blocklistEntries_.erase(it, blocklistEntries_.end());
            removed = true;
        }
    }
    if (removed)
        saveBlocklistToDisk();
    return removed;
}

void PluginScanToolImpl::clearBlocklist() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!blocklistEntries_.empty()) {
            blocklistEntries_.clear();
            changed = true;
        }
    }
    if (changed)
        saveBlocklistToDisk();
}

void PluginScanToolImpl::addToBlocklist(const std::string& formatName, const std::string& pluginId, const std::string& reason) {
    bool changed = false;
    auto id = makeBlocklistId(formatName, pluginId);
    auto now = std::chrono::system_clock::now();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = std::find_if(blocklistEntries_.begin(), blocklistEntries_.end(), [&](const BlocklistEntry& entry) {
            return entry.id == id;
        });
        if (it != blocklistEntries_.end()) {
            it->reason = reason;
            it->timestamp = now;
            changed = true;
        } else {
            blocklistEntries_.push_back(BlocklistEntry{ id, formatName, pluginId, reason, now });
            changed = true;
        }
    }
    if (changed)
        saveBlocklistToDisk();
}

std::string PluginScanToolImpl::makeBlocklistId(const std::string& formatName, const std::string& pluginId) const {
    return formatName + ":" + pluginId;
}

bool PluginScanToolImpl::isBlocklisted(const std::string& formatName, const std::string& pluginId) const {
    auto id = makeBlocklistId(formatName, pluginId);
    std::lock_guard<std::mutex> lock(stateMutex_);
    return std::any_of(blocklistEntries_.begin(), blocklistEntries_.end(), [&](const BlocklistEntry& entry) {
        return entry.id == id;
    });
}

bool PluginScanToolImpl::isBundleBlocklisted(const std::string& formatName,
                                                         const std::filesystem::path& bundlePath) const {
    return isBlocklisted(formatName, bundlePath.lexically_normal().string());
}

remidy_tooling::ScanSessionManager& PluginScanToolImpl::ensureInProcessSessionManager() {
    if (!inProcessSessionManager_)
        inProcessSessionManager_ = std::make_unique<InProcessScanSessionManager>();
    return *inProcessSessionManager_;
}

remidy_tooling::ScanSessionManager& PluginScanToolImpl::ensureRemoteSessionManager() {
#if ANDROID || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    throw std::runtime_error("Remote scanning is unavailable on this platform.");
#else
    if (!remoteSessionManager_)
        remoteSessionManager_ = std::make_unique<RemoteScanSessionManager>();
    return *remoteSessionManager_;
#endif
}

void PluginScanToolImpl::loadBlocklistFromDisk() {
    if (!canPersistBlocklist())
        return;

    std::error_code ec;
    if (!std::filesystem::exists(blocklist_file_, ec))
        return;

    std::ostringstream ss;
    std::ifstream ifs{blocklist_file_};
    if (!ifs)
        return;
    ss << ifs.rdbuf();
    auto content = ss.str();
    if (content.empty())
        return;

    try {
        auto json = choc::json::parse(content);
        auto view = json.getView();
        if (!view.isArray())
            return;
        std::vector<BlocklistEntry> loaded;
        loaded.reserve(view.size());
        for (auto entry : view) {
            BlocklistEntry item{};
            auto id = entry["id"];
            auto format = entry["format"];
            auto plugin = entry["pluginId"];
            auto reason = entry["reason"];
            auto timestamp = entry["timestamp"];
            if (id.isVoid() || format.isVoid() || plugin.isVoid())
                continue;
            item.id = id.toString();
            item.format = format.toString();
            item.pluginId = plugin.toString();
            item.reason = reason.isVoid() ? std::string{} : reason.toString();
            if (!timestamp.isVoid() && timestamp.isInt64()) {
                auto millis = std::chrono::milliseconds(timestamp.getInt64());
                item.timestamp = std::chrono::system_clock::time_point{millis};
            } else {
                item.timestamp = std::chrono::system_clock::now();
            }
            loaded.emplace_back(std::move(item));
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        blocklistEntries_ = std::move(loaded);
    } catch (...) {
        // Ignore malformed blocklist files for now.
    }
}

void PluginScanToolImpl::saveBlocklistToDisk() const {
    if (!canPersistBlocklist())
        return;

    std::vector<BlocklistEntry> snapshot;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot = blocklistEntries_;
    }

    std::vector<choc::value::Value> serialized;
    serialized.reserve(snapshot.size());
    for (const auto& entry : snapshot) {
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(entry.timestamp.time_since_epoch()).count();
        serialized.emplace_back(choc::value::createObject("BlocklistEntry",
                                                          "id", entry.id,
                                                          "format", entry.format,
                                                          "pluginId", entry.pluginId,
                                                          "reason", entry.reason,
                                                          "timestamp", static_cast<int64_t>(millis)));
    }
    auto json = choc::value::createArray(serialized);

    auto parent = blocklist_file_.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent))
        std::filesystem::create_directories(parent);
    std::ofstream ofs{blocklist_file_};
    if (!ofs)
        return;
    ofs << choc::json::toString(json, true);
    syncBrowserFsAsync();
}

bool PluginScanToolImpl::canPersistBlocklist() const {
    return !blocklist_file_.empty();
}

void PluginScanToolImpl::setLastScanError(std::string message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastScanErrorMessage_ = std::move(message);
}

void PluginScanToolImpl::notifyBundleScanStarted(const std::filesystem::path& bundlePath,
                                                             PluginScanObserver* observer) const {
    if (observer && observer->bundleScanStarted)
        observer->bundleScanStarted(bundlePath);
}

void PluginScanToolImpl::notifyBundleScanCompleted(const std::filesystem::path& bundlePath,
                                                               PluginScanObserver* observer) const {
    if (observer && observer->bundleScanCompleted)
        observer->bundleScanCompleted(bundlePath);
}

void PluginScanToolImpl::notifySlowScanStarted(uint32_t totalBundles,
                                                           PluginScanObserver* observer) const {
    if (observer && observer->slowScanStarted)
        observer->slowScanStarted(totalBundles);
}

void PluginScanToolImpl::notifySlowScanCompleted(PluginScanObserver* observer) const {
    if (observer && observer->slowScanCompleted)
        observer->slowScanCompleted();
}

void PluginScanToolImpl::notifyScanError(const std::string& message,
                                                     PluginScanObserver* observer) {
    setLastScanError(message);
    if (observer && observer->errorOccurred)
        observer->errorOccurred(message);
}

bool PluginScanToolImpl::isScanCancellationRequested(PluginScanObserver* observer) const {
    return observer && observer->shouldCancel && observer->shouldCancel();
}

std::unique_ptr<PluginScanTool> PluginScanTool::create() {
    return std::make_unique<PluginScanToolImpl>();
}

} // namespace remidy_tooling
