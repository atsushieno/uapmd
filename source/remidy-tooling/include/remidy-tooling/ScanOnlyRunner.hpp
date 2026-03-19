#pragma once

#include <choc/containers/choc_Value.h>

namespace remidy_tooling {

struct ScanOnlyOptions {
    bool forceRescan = false;
    bool fullVerification = false;
    bool useRemoteScanner = false;
    double bundleTimeoutSeconds = 0.0;
};

// Executes the standalone scan workflow used by CLI tools. On supported platforms
// the resulting JSON document mirrors uapmd-app --scan-only output.
// Returns EXIT_SUCCESS on success, EXIT_FAILURE otherwise.
int runScanOnlyMode(const ScanOnlyOptions& options, choc::value::Value* outReport = nullptr);

}
