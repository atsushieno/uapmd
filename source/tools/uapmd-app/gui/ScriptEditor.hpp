#pragma once

#include <string>
#include <memory>
#include <vector>
#include "../UapmdJSRuntime.hpp"

namespace uapmd::gui {

class ScriptEditor {
    struct ScriptPreset {
        std::string title;
        std::string content;
    };

    struct ScriptFileEntry {
        std::string token;       // from IDocumentProvider::persistHandle()
        std::string displayName; // human-readable filename for UI
    };

    std::vector<char> scriptBuffer_;
    bool isOpen_ = false;
    std::string errorMessage_;
    uapmd::UapmdJSRuntime jsRuntime_;
    std::vector<ScriptPreset> presets_;
    std::vector<ScriptFileEntry> fileHistory_;

public:
    ScriptEditor();

    void show();
    void hide();
    bool isOpen() const { return isOpen_; }

    void render();

private:
    void executeScript();
    void setDefaultScript();
    std::string getJsLibraryPath(const std::string& filename) const;
    void loadScriptFromFile();
    void loadScriptFromHistoryEntry(size_t index);
    void addToHistory(const std::string& token, const std::string& displayName);
    void loadSettings();
    void saveSettings();
    static std::string getSettingsFilePath();
};

} // namespace uapmd::gui
