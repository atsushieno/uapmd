#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../Controller/VirtualMidiDeviceController.hpp"
#include "../VirtualMidiDevices/UapmdMidiDevice.hpp"

namespace uapmd {

    class CommandShell {
        std::unique_ptr<VirtualMidiDeviceController> controller;
        std::unique_ptr<UapmdMidiDevice> device;

    public:
        explicit CommandShell(const std::string& configName);

        static std::unique_ptr<CommandShell> create(int32_t argc, const char** argv);
        int run();
    };

}