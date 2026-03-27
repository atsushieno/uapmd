#pragma once

#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#if !_WIN32
#include <sys/types.h>
#endif

#if !defined(ANDROID) && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#define REMIDY_TOOLING_REMOTE_SCAN_SUPPORTED 1
#else
#define REMIDY_TOOLING_REMOTE_SCAN_SUPPORTED 0
#endif

#include "IpcJsonChannel.hpp"
#include "ScanSessionManager.hpp"
#if REMIDY_TOOLING_REMOTE_SCAN_SUPPORTED == 0
#include "remidy-tooling/detail/PluginScanTool.hpp"
#endif

namespace remidy_tooling {

class PluginScanTool;

#if REMIDY_TOOLING_REMOTE_SCAN_SUPPORTED

class RemoteScanSessionManager final : public ScanSessionManager {
public:
    RemoteScanSessionManager() = default;
    ~RemoteScanSessionManager() override = default;

    void runScan(PluginScanTool& tool,
                 const SlowScanCatalog& catalog,
                 bool requireFastScanning,
                 std::filesystem::path& pluginListCacheFile,
                 bool forceRescan,
                 double bundleTimeoutSeconds,
                 PluginScanObserver* observer) override;

private:
    struct RemoteProcessHandle {
        std::thread waiter;
        std::future<int> exitFuture;
#if _WIN32
        void* processHandle{nullptr};
#else
        pid_t pid{-1};
#endif
    };

    std::string generateToken();
    std::vector<std::string> buildCommandArgs(const std::filesystem::path& executablePath,
                                              uint16_t port,
                                              const std::string& token) const;
    bool launchProcess(const std::vector<std::string>& commandArgs, RemoteProcessHandle& handle);
    void terminateProcess(RemoteProcessHandle& handle);
    bool waitForProcess(RemoteProcessHandle& handle, int& exitCode);
};

#else

class RemoteScanSessionManager final : public ScanSessionManager {
public:
    void runScan(PluginScanTool& tool,
                 const SlowScanCatalog& catalog,
                 bool /*requireFastScanning*/,
                 std::filesystem::path& /*pluginListCacheFile*/,
                 bool /*forceRescan*/,
                 double /*bundleTimeoutSeconds*/,
                 PluginScanObserver* observer) override {
        (void) catalog;
        tool.notifyScanError("Remote scanning is unavailable on this platform.", observer);
    }
};

#endif

} // namespace remidy_tooling
