#pragma once

#include <string>
#include <memory>
#include <vector>
#include <choc/javascript/choc_javascript.h>

namespace uapmd::gui {

class ScriptEditor {
    std::vector<char> scriptBuffer_;
    bool isOpen_ = false;
    std::string errorMessage_;
    choc::javascript::Context jsContext_;

public:
    ScriptEditor();

    void show();
    void hide();
    bool isOpen() const { return isOpen_; }

    void render();

private:
    void initializeJavaScriptContext();
    void executeScript();
    void setDefaultScript();
    std::string getJsLibraryPath (const std::string& filename) const;
};

} // namespace uapmd::gui
