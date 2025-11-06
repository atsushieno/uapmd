
#include <algorithm>
#include <cpptrace/from_current.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef UAPMD_SERVICE_HAS_GUI
#include "Gui/GuiApp.hpp"
#endif
#include "Shell/CommandShell.hpp"

int main(int argc, const char** argv) {
    CPPTRACE_TRY {
        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(std::max(argc - 1, 0)));
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }

        enum class Mode {
            Gui,
            Headless
        };

        std::optional<Mode> requestedMode;
        std::vector<std::string> positional;
        positional.reserve(args.size());
        for (const auto& arg : args) {
            /*if (arg == "--shell" || arg == "--cli" || arg == "--no-gui" || arg == "--headless") {
                requestedMode = Mode::Headless;
                continue;
            }
            if (arg == "--gui") {
                requestedMode = Mode::Gui;
                continue;
            }*/
            positional.push_back(arg);
        }

#ifdef UAPMD_SERVICE_HAS_GUI
        bool guiAvailable = true;
#else
        bool guiAvailable = false;
#endif

        bool runGui = guiAvailable;
        if (requestedMode.has_value()) {
            if (*requestedMode == Mode::Gui) {
                if (!guiAvailable) {
                    std::cerr << "uapmd-service built without GUI support; continuing in headless mode" << std::endl;
                    runGui = false;
                } else {
                    runGui = true;
                }
            } else {
                runGui = false;
            }
        }

        if (!runGui) {
            std::vector<const char*> shellArgv;
            shellArgv.reserve(positional.size() + 1);
            shellArgv.push_back(argv[0]);
            for (const auto& value : positional) {
                shellArgv.push_back(value.c_str());
            }

            auto shell = uapmd::CommandShell::create(static_cast<int32_t>(shellArgv.size()), shellArgv.data());
            return shell->run();
        }

#ifdef UAPMD_SERVICE_HAS_GUI
        uapmd::service::gui::GuiDefaults defaults;
        if (!positional.empty()) defaults.pluginName = positional[0];
        if (positional.size() > 1) defaults.formatName = positional[1];
        if (positional.size() > 2) defaults.apiName = positional[2];
        if (positional.size() > 3) defaults.deviceName = positional[3];

        uapmd::service::gui::GuiApp app;
        return app.run(argc, argv, std::move(defaults));
#else
        (void)argc;
        (void)argv;
        std::cerr << "uapmd-service can only run in headless mode in this build" << std::endl;
        return EXIT_FAILURE;
#endif
    } CPPTRACE_CATCH(const std::exception& ex) {
        std::cerr << "Exception in uapmd-service: " << ex.what() << std::endl;
        cpptrace::from_current_exception().print();
        return EXIT_FAILURE;
    }
}
