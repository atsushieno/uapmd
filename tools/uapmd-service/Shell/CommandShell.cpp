
#include "CommandShell.hpp"

#include <iostream>

namespace uapmd {

    CommandShell::CommandShell(std::string configName) : config_name(std::move(configName)) {
    }

    std::unique_ptr<CommandShell> CommandShell::create(int32_t argc, const char** argv) {
        return std::make_unique<CommandShell>(argc < 2 ? "" : argv[1]);
    }

    int CommandShell::run() {
        controller = std::make_unique<VirtualMidiDeviceController>();
        const auto& deviceName = config_name.size() ? config_name : "Serum2"; // demo
        std::cerr << "Starting " << deviceName << std::endl;
        device = controller->createDevice(deviceName, "UAPMD Project", "0.1");

        device->start();

        std::cerr << "Type [CR] to quit." << std::endl;
        std::cin.get();

        device->stop();

        return 0;
    }

}
