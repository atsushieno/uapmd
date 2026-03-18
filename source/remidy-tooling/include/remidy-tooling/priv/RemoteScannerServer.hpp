#pragma once

#include <cstdint>
#include <string>

namespace remidy_tooling {

struct RemoteScannerServerOptions {
    std::string host;
    uint16_t port = 0;
    std::string token;
};

// Runs the remote scanner server loop on supported desktop platforms.
// Returns 0 on success, non-zero on error.
int runRemoteScannerServer(const RemoteScannerServerOptions& options);

}

