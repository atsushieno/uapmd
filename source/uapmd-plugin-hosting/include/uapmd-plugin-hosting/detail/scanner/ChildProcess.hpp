#pragma once

namespace uapmd_plugin_hosting {

class ChildProcess {
public:
    ChildProcess(int argc, char** argv) noexcept
        : argc_(argc), argv_(argv) {}
    virtual ~ChildProcess() = default;

    virtual bool matches() = 0;
    virtual int process() = 0;

protected:
    int argc_;
    char** argv_;
};

} // namespace uapmd_plugin_hosting
