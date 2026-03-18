#pragma once

#include <cstdint>
#include <string>

#include "remidy-tooling/priv/ChildProcess.hpp"
#include "remidy-tooling/priv/RemoteScannerServer.hpp"

namespace remidy_tooling {

class RemotePluginScannerProcess final : public ChildProcess {
public:
    RemotePluginScannerProcess(int argc, char** argv);

    bool matches() override;
    int process() override;

private:
    struct Options {
        bool scanOnly = false;
        bool ipcClient = false;
        std::string ipcHost = "127.0.0.1";
        uint16_t ipcPort = 0;
        std::string ipcToken;
    };

    void ensureParsed();

    bool parsed_ = false;
    Options options_{};
};

} // namespace remidy_tooling
