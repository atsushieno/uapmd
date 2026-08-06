#pragma once

#include <functional>
#include <string>

#include <imgui.h>

namespace uapmd::gui {

class MixerMonitorWindow {
public:
    struct Callbacks {
        std::function<void(const std::string&, ImVec2)> setChildSize;
        std::function<void(const std::string&)> updateChildSizeState;
    };

    MixerMonitorWindow() = default;
    explicit MixerMonitorWindow(Callbacks callbacks);

    void setCallbacks(Callbacks callbacks);

    void toggle();
    void hide();
    bool isVisible() const { return visible_; }

    void render(float uiScale);

private:
    Callbacks callbacks_{};
    bool visible_{false};
};

} // namespace uapmd::gui
