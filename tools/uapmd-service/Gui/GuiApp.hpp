#pragma once

#include "MainWindow.hpp"

namespace uapmd::service::gui {

class GuiApp {
public:
    GuiApp() = default;

    int run(int argc, const char** argv, GuiDefaults defaults);
};

} // namespace uapmd::service::gui
