#pragma once

#include <string>
#include <memory>
#include <TextEditor.h>
#include <choc/javascript/choc_javascript.h>

namespace uapmd::gui {

class ScriptEditor {
    TextEditor editor_;
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
