#include "remidy-tooling/priv/RemotePluginScannerProcess.hpp"

#include <cstdlib>
#include <string>

namespace remidy_tooling {

RemotePluginScannerProcess::RemotePluginScannerProcess(int argc, char** argv)
    : ChildProcess(argc, argv) {}

bool RemotePluginScannerProcess::matches() {
    ensureParsed();
    return options_.scanOnly && options_.ipcClient;
}

int RemotePluginScannerProcess::process() {
    ensureParsed();
    if (!matches())
        return EXIT_FAILURE;

    RemoteScannerServerOptions serverOptions{
        .host = options_.ipcHost,
        .port = options_.ipcPort,
        .token = options_.ipcToken
    };
    return runRemoteScannerServer(serverOptions);
}

void RemotePluginScannerProcess::ensureParsed() {
    if (parsed_)
        return;
    parsed_ = true;
    for (int i = 1; i < argc_; ++i) {
        const char* rawArg = argv_[i];
        if (!rawArg)
            continue;
        std::string arg(rawArg);
        if (arg == "--scan-only") {
            options_.scanOnly = true;
            continue;
        }
        if (arg == "--ipc-client") {
            options_.ipcClient = true;
            continue;
        }
        if (arg == "--ipc-host" && i + 1 < argc_) {
            const char* value = argv_[++i];
            if (value && *value)
                options_.ipcHost = value;
            continue;
        }
        if (arg == "--ipc-port" && i + 1 < argc_) {
            const char* value = argv_[++i];
            if (value) {
                try {
                    int port = std::stoi(value);
                    if (port >= 0 && port <= 65535)
                        options_.ipcPort = static_cast<uint16_t>(port);
                    else
                        options_.ipcPort = 0;
                } catch (...) {
                    options_.ipcPort = 0;
                }
            }
            continue;
        }
        if (arg == "--ipc-token" && i + 1 < argc_) {
            const char* value = argv_[++i];
            if (value)
                options_.ipcToken = value;
            continue;
        }
    }
}

} // namespace remidy_tooling
