#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace remidy_tooling {

struct ScanOnlyOptions {
    bool forceRescan = false;
    bool fullVerification = false;
    bool useRemoteScanner = false;
    double bundleTimeoutSeconds = 0.0;
};

struct ScannedPluginEntry {
    std::string format;
    std::string id;
    std::string name;
    std::string vendor;
    std::string url;
    std::string bundle;
};

struct ScanVerificationFailure {
    std::string format;
    std::string pluginId;
    std::string name;
    std::string message;
};

struct ScanVerificationReport {
    bool enabled = false;
    size_t attempted = 0;
    size_t succeeded = 0;
    std::vector<ScanVerificationFailure> failures;
};

struct ScanOnlyReport {
    bool success = false;
    bool forceRescan = false;
    std::filesystem::path cacheFile;
    std::vector<ScannedPluginEntry> plugins;
    ScanVerificationReport fullVerification;
    std::string error; // empty when success is true
};

// Executes the standalone scan workflow used by CLI tools. On supported platforms
// the resulting report mirrors uapmd-app --scan-only output.
// Returns EXIT_SUCCESS on success, EXIT_FAILURE otherwise.
int runScanOnlyMode(const ScanOnlyOptions& options, ScanOnlyReport* outReport = nullptr);

}
