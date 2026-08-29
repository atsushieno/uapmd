#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace uapmd_plugin_hosting {

struct RemoteScannerServerOptions {
    std::string host;
    uint16_t port = 0;
    std::string token;
};

// Runs the remote scanner server loop on supported desktop platforms.
// Returns 0 on success, non-zero on error.
int runRemoteScannerServer(const RemoteScannerServerOptions& options);

// Overrides the executable the remote scanner launches for its child process.
//
// By default the host relaunches *itself* with `--scan-only --ipc-client ...`,
// which works for uapmd-app because its own main() dispatches those arguments.
// An embedder whose process cannot serve that role — a JVM or browser host, where
// the executable is the runtime launcher rather than the app — has no way to scan
// out of process at all without this. Point it at a standalone scanner that
// understands the same arguments, such as `uapmd-scan`.
//
// Pass an empty path to restore the default.
void setRemoteScannerExecutable(std::filesystem::path path);
std::filesystem::path remoteScannerExecutable();

}

