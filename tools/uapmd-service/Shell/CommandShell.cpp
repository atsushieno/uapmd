
#include "CommandShell.hpp"

#include <iostream>

namespace uapmd {

    CommandShell::CommandShell(
        std::string pluginName,
        std::string formatName
    ) : plugin_name(std::move(pluginName)),
        format_name(std::move(formatName))
    {
    }

    std::unique_ptr<CommandShell> CommandShell::create(int32_t argc, const char** argv) {
        return std::make_unique<CommandShell>(argc < 2 ? "" : argv[1], argc < 3 ? "" : argv[2]);
    }

    int CommandShell::run() {
        controller = std::make_unique<VirtualMidiDeviceController>();
        const auto& deviceName = plugin_name.size() ? plugin_name : "UAPMD (Any Plugin)";
        Logger::global()->logInfo("Starting %s", deviceName.c_str());
        device = controller->createDevice(deviceName, "UAPMD Project", "0.1");
        device->addPluginTrack(plugin_name, format_name);

        device->start();

        std::cerr << "Type [CR] to quit." << std::endl;
        std::cin.get();

        device->stop();

        return 0;
    }

}
