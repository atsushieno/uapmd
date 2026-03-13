#pragma once

#include <string>

namespace uapmd {

struct ScanOnlyOptions {
    bool forceRescan = false;
    bool fullVerification = false;
};

int runScanOnlyMode(const ScanOnlyOptions& options);

}
