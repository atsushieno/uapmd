#pragma once

namespace uapmd::gui {

class ScriptEditor {
    bool open_ = false;
public:
    ScriptEditor() = default;
    void show() { open_ = true; }
    void hide() { open_ = false; }
    bool isOpen() const { return open_; }
    void render() {}
};

}

