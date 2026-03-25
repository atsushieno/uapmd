#include "RemoteScanSessionManager.hpp"
#include "../ScanConstants.hpp"

#if REMIDY_TOOLING_REMOTE_SCAN_SUPPORTED

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <sstream>
#include <vector>
#include <cctype>

#include <choc/containers/choc_Value.h>

#include <cpplocate/cpplocate.h>
#if _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "remidy-tooling/priv/PluginScanTool.hpp"
#include "ScannerProtocol.hpp"

namespace remidy_tooling {

namespace {
constexpr int kConnectionTimeoutMs = 10000;
constexpr int kMessageTimeoutMs = 300000; // allow up to 5 minutes between progress events (dialogs can block the worker)

std::string makeBundleLabel(const std::string& format, const std::filesystem::path& bundlePath) {
    auto filename = bundlePath.filename().string();
    if (filename.empty())
        filename = bundlePath.string();
    if (filename.empty())
        return format;
    return std::format("{}: {}", format, filename);
}

std::string normalizeBundleKey(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal().generic_string();
#if _WIN32
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return normalized;
}

void pruneEmptyEntries(SlowScanCatalog& catalog) {
    catalog.erase(std::remove_if(catalog.begin(),
                                 catalog.end(),
                                 [](const SlowScanEntry& entry) {
                                     return entry.bundles.empty() || entry.format == nullptr;
                                 }),
                  catalog.end());
}

void removeBundleFromCatalog(SlowScanCatalog& catalog,
                             const std::string& formatName,
                             const std::filesystem::path& bundlePath) {
    if (formatName.empty())
        return;
    auto targetKey = normalizeBundleKey(bundlePath);
    for (auto entryIt = catalog.begin(); entryIt != catalog.end();) {
        auto* format = entryIt->format;
        if (!format || format->name() != formatName) {
            ++entryIt;
            continue;
        }
        auto& bundles = entryIt->bundles;
        auto newEnd = std::remove_if(bundles.begin(),
                                     bundles.end(),
                                     [&](const std::filesystem::path& candidate) {
                                         return normalizeBundleKey(candidate) == targetKey;
                                     });
        if (newEnd != bundles.end())
            bundles.erase(newEnd, bundles.end());
        if (bundles.empty())
            entryIt = catalog.erase(entryIt);
        else
            ++entryIt;
        break;
    }
}

size_t countBundles(const SlowScanCatalog& catalog) {
    size_t total = 0;
    for (const auto& entry : catalog)
        total += entry.bundles.size();
    return total;
}

#if _WIN32
std::string quoteWindowsArg(const std::string& arg) {
    if (arg.empty())
        return "\"\"";
    bool needsQuotes = false;
    for (char c : arg) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes)
        return arg;
    std::string result;
    result.push_back('"');
    int backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            result.append(backslashes, '\\');
            backslashes = 0;
        }
        result.push_back(c);
    }
    result.append(backslashes * 2, '\\');
    result.push_back('"');
    return result;
}

std::string buildWindowsCommandLine(const std::vector<std::string>& args) {
    std::ostringstream command;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0)
            command << ' ';
        command << quoteWindowsArg(args[i]);
    }
    return command.str();
}
#endif

} // namespace

std::string RemoteScanSessionManager::generateToken() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << dist(rng);
    return ss.str();
}

std::vector<std::string> RemoteScanSessionManager::buildCommandArgs(const std::filesystem::path& executablePath,
                                                                    uint16_t port,
                                                                    const std::string& token) const {
    std::vector<std::string> args;
    args.emplace_back(executablePath.string());
    args.emplace_back("--scan-only");
    args.emplace_back("--ipc-client");
    args.emplace_back("--ipc-host");
    args.emplace_back("127.0.0.1");
    args.emplace_back("--ipc-port");
    args.emplace_back(std::to_string(port));
    args.emplace_back("--ipc-token");
    args.emplace_back(token);
    return args;
}

bool RemoteScanSessionManager::launchProcess(const std::vector<std::string>& commandArgs,
                                             RemoteProcessHandle& handle) {
    if (commandArgs.empty())
        return false;

    std::promise<int> exitPromise;
    auto exitFuture = exitPromise.get_future();

#if _WIN32
    auto commandLine = buildWindowsCommandLine(commandArgs);
    std::vector<char> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back('\0');

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessA(commandArgs[0].c_str(),
                        commandBuffer.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startupInfo,
                        &processInfo)) {
        return false;
    }

    handle.processHandle = processInfo.hProcess;
    CloseHandle(processInfo.hThread);

    auto processHandleCopy = static_cast<HANDLE>(handle.processHandle);
    try {
        handle.waiter = std::thread([processHandleCopy, promise = std::move(exitPromise)]() mutable {
            DWORD exitCode = static_cast<DWORD>(-1);
            if (processHandleCopy) {
                WaitForSingleObject(processHandleCopy, INFINITE);
                if (!GetExitCodeProcess(processHandleCopy, &exitCode))
                    exitCode = static_cast<DWORD>(-1);
            }
            promise.set_value(static_cast<int>(exitCode));
        });
    } catch (...) {
        if (processHandleCopy) {
            TerminateProcess(processHandleCopy, 1);
            WaitForSingleObject(processHandleCopy, INFINITE);
            CloseHandle(processHandleCopy);
        }
        throw;
    }
#else
    std::vector<char*> argv;
    argv.reserve(commandArgs.size() + 1);
    for (const auto& arg : commandArgs)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int spawnResult = posix_spawn(&pid,
                                  commandArgs[0].c_str(),
                                  nullptr,
                                  nullptr,
                                  argv.data(),
                                  environ);
    if (spawnResult != 0)
        return false;
    handle.pid = pid;
    try {
        handle.waiter = std::thread([pid, promise = std::move(exitPromise)]() mutable {
            int result = -1;
            if (pid > 0) {
                int status = 0;
                if (::waitpid(pid, &status, 0) >= 0) {
                    if (WIFEXITED(status))
                        result = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status))
                        result = -WTERMSIG(status);
                }
            }
            promise.set_value(result);
        });
    } catch (...) {
        if (pid > 0) {
            kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        throw;
    }
#endif

    handle.exitFuture = std::move(exitFuture);
    return true;
}

void RemoteScanSessionManager::terminateProcess(RemoteProcessHandle& handle) {
#if _WIN32
    auto processHandle = static_cast<HANDLE>(handle.processHandle);
    if (processHandle)
        TerminateProcess(processHandle, static_cast<UINT>(kScanTimeoutExitCode));
#else
    if (handle.pid > 0)
        kill(handle.pid, SIGKILL);
#endif
}

bool RemoteScanSessionManager::waitForProcess(RemoteProcessHandle& handle, int& exitCode) {
    if (handle.waiter.joinable())
        handle.waiter.join();
    exitCode = handle.exitFuture.valid() ? handle.exitFuture.get() : -1;
#if _WIN32
    auto processHandle = static_cast<HANDLE>(handle.processHandle);
    if (processHandle) {
        CloseHandle(processHandle);
        handle.processHandle = nullptr;
    }
#else
    handle.pid = -1;
#endif
    return exitCode == 0;
}

void RemoteScanSessionManager::runScan(PluginScanTool& tool,
                                       const SlowScanCatalog& catalogPlan,
                                       bool requireFastScanning,
                                       std::filesystem::path& pluginListCacheFile,
                                       bool forceRescan,
                                       double bundleTimeoutSeconds,
                                       PluginScanObserver* observer) {
    if (catalogPlan.empty())
        return;

    SlowScanCatalog remaining = catalogPlan;
    pruneEmptyEntries(remaining);
    if (remaining.empty())
        return;

    bool pendingForceRescan = forceRescan;

    while (!remaining.empty()) {
        ipc::TcpServer server;
        if (!server.listen(0)) {
            tool.notifyScanError("Failed to create IPC server for remote scanning.", observer);
            return;
        }

        auto exePathString = cpplocate::getExecutablePath();
        if (exePathString.empty()) {
            tool.notifyScanError("Unable to determine executable path for remote scanning.", observer);
            return;
        }

        auto token = generateToken();
        auto commandArgs = buildCommandArgs(exePathString, server.port(), token);

        RemoteProcessHandle processHandle{};
        if (!launchProcess(commandArgs, processHandle)) {
            tool.notifyScanError("Failed to launch remote scanner process.", observer);
            return;
        }

        auto socketOpt = server.accept(kConnectionTimeoutMs);
        if (!socketOpt.has_value()) {
            tool.notifyScanError("Remote scanner failed to connect.", observer);
            int exitCode = -1;
            terminateProcess(processHandle);
            waitForProcess(processHandle, exitCode);
            return;
        }

        ipc::IpcJsonChannel channel(std::move(*socketOpt));

        auto hello = channel.receive(kConnectionTimeoutMs);
        if (!hello.has_value() || hello->type != ipc::kScannerMsgHello) {
            tool.notifyScanError("Remote scanner did not send handshake.", observer);
            int exitCode = -1;
            terminateProcess(processHandle);
            waitForProcess(processHandle, exitCode);
            return;
        }
        auto helloToken = hello->payload["token"];
        if (helloToken.isVoid() || helloToken.toString() != token) {
            tool.notifyScanError("Remote scanner authentication failed.", observer);
            int exitCode = -1;
            terminateProcess(processHandle);
            waitForProcess(processHandle, exitCode);
            return;
        }

        choc::value::Value payload = choc::value::createObject("StartScan");
        payload.setMember("forceRescan", pendingForceRescan);
        payload.setMember("requireFastScanning", requireFastScanning);
        payload.setMember("cacheFile", pluginListCacheFile.string());
        auto planArray = choc::value::createEmptyArray();
        for (const auto& entry : remaining) {
            if (!entry.format)
                continue;
            choc::value::Value planEntry = choc::value::createObject("SlowScanEntry");
            planEntry.setMember("format", entry.format->name());
            auto bundlesArray = choc::value::createEmptyArray();
            for (const auto& bundle : entry.bundles)
                bundlesArray.addArrayElement(bundle.string());
            planEntry.setMember("bundles", std::move(bundlesArray));
            planArray.addArrayElement(planEntry);
        }
        payload.setMember("slowCatalog", std::move(planArray));
        payload.setMember("totalBundles", static_cast<int32_t>(countBundles(remaining)));
        payload.setMember("timeoutSeconds", bundleTimeoutSeconds);
        ipc::IpcMessage startMsg{
            .type = ipc::kScannerMsgStartScan,
            .requestId = "start",
            .payload = payload
        };
        if (!channel.send(startMsg)) {
            tool.notifyScanError("Failed to send start command to remote scanner.", observer);
            int exitCode = -1;
            terminateProcess(processHandle);
            waitForProcess(processHandle, exitCode);
            return;
        }
        pendingForceRescan = false;

        bool finished = false;
        bool remoteSucceeded = false;
        std::filesystem::path cachePath{};
        bool cancelSent = false;
        bool bundleTimedOut = false;
        std::optional<std::filesystem::path> activeBundlePath;
        std::string activeBundleFormat;
        std::string activeBundleLabel;
        std::optional<std::chrono::steady_clock::time_point> activeBundleStart;

        auto blocklistActiveBundle = [&](const std::string& reason) {
            if (activeBundleFormat.empty() || !activeBundlePath.has_value())
                return;
            auto normalized = activeBundlePath->lexically_normal().string();
            tool.addToBlocklist(activeBundleFormat, normalized, reason);
            removeBundleFromCatalog(remaining, activeBundleFormat, *activeBundlePath);
        };

        while (!finished) {
            if (bundleTimeoutSeconds > 0.0 && activeBundleStart.has_value()) {
                auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - *activeBundleStart).count();
                if (elapsed > bundleTimeoutSeconds) {
                    bundleTimedOut = true;
                    std::string label = !activeBundleLabel.empty()
                                            ? activeBundleLabel
                                            : (activeBundlePath.has_value() ? activeBundlePath->string() : std::string{"Bundle"});
                    auto message = std::format("{} timed out after {:.1f} seconds", label, bundleTimeoutSeconds);
                    tool.notifyScanError(message, observer);
                    blocklistActiveBundle(message);
                    channel.close();
                    terminateProcess(processHandle);
                    finished = true;
                    break;
                }
            }

            if (!cancelSent && tool.isScanCancellationRequested(observer)) {
                ipc::IpcMessage cancelMsg{
                    .type = ipc::kScannerMsgCancelScan,
                    .requestId = "cancel",
                    .payload = choc::value::Value{}
                };
                cancelSent = channel.send(cancelMsg);
                if (!cancelSent) {
                    tool.notifyScanError("Failed to send cancel command to remote scanner.", observer);
                    break;
                }
            }

            bool timedOut = false;
            auto message = channel.receive(250, timedOut);
            if (timedOut)
                continue;
            if (!message.has_value()) {
                if (tool.isScanCancellationRequested(observer))
                    tool.notifyScanError("Remote scanning canceled.", observer);
                else
                    tool.notifyScanError("Remote scanner disconnected unexpectedly.", observer);
                if (!tool.isScanCancellationRequested(observer))
                    blocklistActiveBundle("Remote scanner disconnected unexpectedly.");
                break;
            }

            if (message->type == ipc::kScannerMsgBundleStarted) {
                auto fmtValue = message->payload["format"];
                auto bundleValue = message->payload["bundlePath"];
                auto fmt = fmtValue.isVoid() ? std::string{} : fmtValue.toString();
                auto bundle = bundleValue.isVoid() ? std::filesystem::path{} : std::filesystem::path(bundleValue.toString());
                activeBundlePath = bundle;
                activeBundleFormat = fmt;
                activeBundleLabel = makeBundleLabel(fmt, bundle);
                activeBundleStart = std::chrono::steady_clock::now();
                tool.notifyBundleScanStarted(bundle, observer);
            } else if (message->type == ipc::kScannerMsgBundleFinished) {
                auto fmtValue = message->payload["format"];
                auto bundleValue = message->payload["bundlePath"];
                auto fmt = fmtValue.isVoid() ? std::string{} : fmtValue.toString();
                auto bundle = bundleValue.isVoid() ? std::filesystem::path{} : std::filesystem::path(bundleValue.toString());
                tool.notifyBundleScanCompleted(bundle, observer);
                removeBundleFromCatalog(remaining, fmt, bundle);
                activeBundlePath.reset();
                activeBundleFormat.clear();
                activeBundleLabel.clear();
                activeBundleStart.reset();
            } else if (message->type == ipc::kScannerMsgBundleTotals) {
                continue;
            } else if (message->type == ipc::kScannerMsgScanResult) {
                auto successVal = message->payload["success"];
                bool success = !successVal.isVoid() && successVal.getBool();
                auto cacheVal = message->payload["cacheFile"];
                if (!cacheVal.isVoid())
                    cachePath = std::filesystem::path(cacheVal.toString());
                if (success) {
                    remoteSucceeded = true;
                } else {
                    auto errorVal = message->payload["error"];
                    if (!errorVal.isVoid())
                        tool.notifyScanError(errorVal.toString(), observer);
                    auto failedFormatVal = message->payload["failedBundleFormat"];
                    auto failedPathVal = message->payload["failedBundlePath"];
                    auto failedReasonVal = message->payload["failedBundleReason"];
                    if (!failedFormatVal.isVoid() && !failedPathVal.isVoid()) {
                        std::string reason = !failedReasonVal.isVoid() ? failedReasonVal.toString()
                                                                       : std::string{"Remote scanning failed."};
                        std::filesystem::path failedPath{failedPathVal.toString()};
                        tool.addToBlocklist(failedFormatVal.toString(), failedPath.lexically_normal().string(), reason);
                        removeBundleFromCatalog(remaining, failedFormatVal.toString(), failedPath);
                    }
                }
                finished = true;
            }
        }

        int exitCode = -1;
        waitForProcess(processHandle, exitCode);

        auto cacheLoadPath = !cachePath.empty() ? cachePath : pluginListCacheFile;
        if (!cacheLoadPath.empty() && std::filesystem::exists(cacheLoadPath)) {
            tool.catalog().clear();
            tool.catalog().load(cacheLoadPath);
            pluginListCacheFile = cacheLoadPath;
        }

        if (bundleTimedOut) {
            continue;
        }

        if (!remoteSucceeded) {
            if (tool.lastScanError().empty())
                tool.notifyScanError("Remote scanning failed.", observer);
            return;
        }
    }
}

} // namespace remidy_tooling

#endif // REMIDY_TOOLING_REMOTE_SCAN_SUPPORTED
