#pragma once

#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd_app_gui {

class AddinManagerWindow {
public:
    explicit AddinManagerWindow(uapmd::AddinManager& runtime);

    void show();
    void hide();
    bool isOpen() const noexcept;
    void render(float uiScale);

private:
    uapmd::AddinManager& runtime_;
    bool open_ = false;
};

} // namespace uapmd_app_gui
