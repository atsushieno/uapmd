
#include "CommandShell.hpp"

#include <iostream>

namespace uapmd {

    CommandShell::CommandShell(
        std::string apiName,
        std::string pluginName,
        std::string formatName
    ) : api_name(std::move(apiName)),
        plugin_name(std::move(pluginName)),
        format_name(std::move(formatName))
    {
    }

    std::unique_ptr<CommandShell> CommandShell::create(int32_t argc, const char** argv) {
        return std::make_unique<CommandShell>(argc < 4 ? "" : argv[3], argc < 2 ? "" : argv[1], argc < 3 ? "" : argv[2]);
    }

    int CommandShell::run() {
        remidy::EventLoop::initializeOnUIThread();

        const auto& deviceName = plugin_name.size() ? plugin_name : "UAPMD (Any Plugin)";
        Logger::global()->logInfo("Starting %s", deviceName.c_str());

        // FIXME: run in realtime audio (priority) thread
        std::thread t([&] {
            controller = std::make_unique<VirtualMidiDeviceController>();
            device = controller->createDevice(api_name, deviceName, "UAPMD Project", "0.1");
            device->addPluginTrack(plugin_name, format_name);

            device->start();
        });

        std::thread t2([&] {
            std::cerr << "Type [CR] to quit." << std::endl;
            std::cin.get();
            remidy::EventLoop::stop();
        });

        remidy::EventLoop::start();

        t2.join();
        t.join();
        device->stop();

        return 0;
    }

}
