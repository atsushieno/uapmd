#pragma once

#include <string>
#include <memory>
#include <vector>
#include "../UapmdJSRuntime.hpp"

namespace uapmd::gui {

class ScriptEditor {
    std::vector<char> scriptBuffer_;
    bool isOpen_ = false;
    std::string errorMessage_;
    uapmd::UapmdJSRuntime jsRuntime_;

public:
    ScriptEditor();

    void show();
    void hide();
    bool isOpen() const { return isOpen_; }

    void render();

private:
    void executeScript();
    void setDefaultScript();
    std::string getJsLibraryPath (const std::string& filename) const;
};

} // namespace uapmd::gui
