#include "ScriptEditor.hpp"
#include "../AppModel.hpp"
#include <imgui.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// cpplocate is desktop-only: excluded on Android, Emscripten, and iOS.
#if !ANDROID && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#include <cpplocate/cpplocate.h>
#endif
#include <choc/text/choc_JSON.h>
#include <AppScripts.h>
#include <AppJsLib.h>
#include <ResEmbed/ResEmbed.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>

namespace uapmd::gui {

ScriptEditor::ScriptEditor()
{
    // Initialize buffer with a large size for the text editor
    scriptBuffer_.resize(65536, '\0');

    // Register built-in script presets from embedded resources.
    // New preset .js files under scripts/ are embedded automatically unless
    // they are reserved bridge library modules listed under AppJsLib.
    for (auto& [filename, data] : ResEmbed::getCategory ("AppScripts"))
    {
        // Strip the .js extension to form the display title
        auto title = filename.ends_with (".js")
            ? filename.substr (0, filename.size() - 3)
            : filename;
        presets_.push_back ({title,
            std::string (reinterpret_cast<const char*> (data.data()), data.size())});
    }

    // Restore file history from settings
    loadSettings();
}

void ScriptEditor::show()
{
    isOpen_ = true;
}

void ScriptEditor::hide()
{
    isOpen_ = false;
}

void ScriptEditor::render()
{
    if (! isOpen_)
        return;

    ImGui::SetNextWindowSize (ImVec2 (640, 600), ImGuiCond_FirstUseEver);

    if (ImGui::Begin ("Script Editor", &isOpen_))
    {
        // Calculate available height for the text editor, leaving space for buttons and error messages
        float buttonHeight = ImGui::GetFrameHeightWithSpacing() * 2;  // Space for buttons
        float errorHeight = errorMessage_.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * 3;  // Space for error message
        float availableHeight = ImGui::GetContentRegionAvail().y - buttonHeight - errorHeight;

        // Render the text editor with constrained height using ImGui's built-in multiline input
        ImGui::InputTextMultiline("##ScriptEditor", scriptBuffer_.data(), scriptBuffer_.size(),
                                   ImVec2(600, availableHeight),
                                   ImGuiInputTextFlags_AllowTabInput);

        ImGui::Separator();

        // Run button
        if (ImGui::Button ("Run"))
            executeScript();

        ImGui::SameLine();

        // Preset menu button — loads a built-in script preset into the editor
        if (ImGui::Button ("Preset"))
            ImGui::OpenPopup ("preset_popup");

        if (ImGui::BeginPopup ("preset_popup"))
        {
            for (auto& preset : presets_)
            {
                if (ImGui::MenuItem (preset.title.c_str()))
                {
                    auto len = std::min (preset.content.size(), scriptBuffer_.size() - 1);
                    std::memcpy (scriptBuffer_.data(), preset.content.c_str(), len);
                    scriptBuffer_[len] = '\0';
                    errorMessage_.clear();
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        // Load menu button — opens a file or picks from recent file history
        if (ImGui::Button ("Load"))
            ImGui::OpenPopup ("load_popup");

        if (ImGui::BeginPopup ("load_popup"))
        {
            if (ImGui::MenuItem ("Open File..."))
                loadScriptFromFile();

            if (! fileHistory_.empty())
            {
                ImGui::Separator();
                for (size_t i = 0; i < fileHistory_.size(); ++i)
                    if (ImGui::MenuItem (fileHistory_[i].displayName.c_str()))
                        loadScriptFromHistoryEntry (i);
            }

            ImGui::EndPopup();
        }

        // Display error message if any
        if (! errorMessage_.empty())
        {
            ImGui::Separator();
            ImGui::TextColored (ImVec4 (1.0f, 0.3f, 0.3f, 1.0f), "Error:");
            ImGui::TextWrapped ("%s", errorMessage_.c_str());
        }
    }

    ImGui::End();
}

void ScriptEditor::executeScript()
{
    errorMessage_.clear();

    try
    {
        auto scriptText = std::string(scriptBuffer_.data());
        auto& jsContext = jsRuntime_.context();

        // Check if the script contains import statements (for module support)
        bool hasImports = scriptText.find ("import") != std::string::npos;

        if (hasImports)
        {
            // Use runModule for scripts with imports
            bool completed = false;
            std::string lastError;
            choc::value::Value lastResult;

            jsContext.runModule (scriptText,
                // Module resolver — tries embedded libraries first (all platforms),
                // then falls back to the filesystem on desktop for dev convenience.
                [this] (std::string_view modulePath) -> std::optional<std::string>
                {
                    auto name = std::string (modulePath);
                    auto withExt = name.ends_with (".js") ? name : name + ".js";

                    if (auto data = ResEmbed::get (withExt, "AppJsLib"))
                        return std::string (reinterpret_cast<const char*> (data.data()), data.size());

                    // Fallback: load from filesystem (desktop dev workflow)
                    auto fullPath = getJsLibraryPath (name);

                    if (fullPath.empty())
                    {
                        std::cerr << "[JS] Module not found: " << modulePath << std::endl;
                        return std::nullopt;
                    }

                    std::ifstream file (fullPath);

                    if (! file.is_open())
                    {
                        std::cerr << "[JS] Failed to open module: " << fullPath << std::endl;
                        return std::nullopt;
                    }

                    std::stringstream buffer;
                    buffer << file.rdbuf();
                    return buffer.str();
                },
                // Completion handler
                [&completed, &lastError, &lastResult] (const std::string& error, const choc::value::ValueView& result)
                {
                    completed = true;

                    if (! error.empty())
                    {
                        lastError = error;
                    }
                    else
                    {
                        lastResult = choc::value::Value (result);
                    }
                });

            // Wait a bit for async completion (simple approach)
            // In a real application, you'd want a proper event loop
            std::this_thread::sleep_for (std::chrono::milliseconds (100));

            if (! lastError.empty())
            {
                throw std::runtime_error (lastError);
            }

            if (lastResult.isVoid())
            {
                std::cout << "[JS] Module executed successfully" << std::endl;
            }
            else
            {
                std::cout << "[JS] Module result: " << choc::json::toString (lastResult, true) << std::endl;
            }
        }
        else
        {
            // Use evaluateExpression for simple scripts without imports
            auto result = jsContext.evaluateExpression (scriptText);

            if (result.isVoid())
            {
                std::cout << "[JS] Script executed successfully (no return value)" << std::endl;
            }
            else
            {
                std::cout << "[JS] Result: " << choc::json::toString (result, true) << std::endl;
            }
        }
    }
    catch (const std::exception& e)
    {
        errorMessage_ = std::string ("JavaScript error: ") + e.what();
        std::cerr << errorMessage_ << std::endl;
    }
}

void ScriptEditor::setDefaultScript()
{
    if (presets_.empty())
        return;
    auto& content = presets_[0].content;
    auto len = std::min (content.size(), scriptBuffer_.size() - 1);
    std::memcpy (scriptBuffer_.data(), content.c_str(), len);
    scriptBuffer_[len] = '\0';
}

void ScriptEditor::loadScriptFromFile()
{
    auto* provider = uapmd::AppModel::instance().documentProvider();
    if (! provider)
        return;

    std::vector<uapmd::DocumentFilter> filters{
        {"JavaScript Files", {}, {"*.js"}},
        {"All Files",        {}, {"*"}}
    };

    provider->pickOpenDocuments (filters, false,
        [this, provider] (uapmd::DocumentPickResult result)
        {
            if (! result.success || result.handles.empty())
                return;

            auto handle = result.handles[0];

            provider->readDocument (handle,
                [this, provider, handle] (uapmd::DocumentIOResult ioResult, std::vector<uint8_t> data)
                {
                    if (! ioResult.success)
                    {
                        errorMessage_ = "Failed to read file: " + ioResult.error;
                        return;
                    }

                    auto len = std::min (data.size(), scriptBuffer_.size() - 1);
                    std::memcpy (scriptBuffer_.data(), data.data(), len);
                    scriptBuffer_[len] = '\0';
                    errorMessage_.clear();

                    auto token = provider->persistHandle (handle);
                    if (! token.empty())
                        addToHistory (token, handle.display_name);
                });
        });
}

void ScriptEditor::loadScriptFromHistoryEntry (size_t index)
{
    if (index >= fileHistory_.size())
        return;

    auto token = fileHistory_[index].token;

    auto* provider = uapmd::AppModel::instance().documentProvider();
    if (! provider)
        return;

    auto handle = provider->restoreHandle (token);
    if (! handle)
    {
        // File is no longer accessible — remove stale entry
        fileHistory_.erase (fileHistory_.begin() + static_cast<ptrdiff_t> (index));
        saveSettings();
        return;
    }

    provider->readDocument (*handle,
        [this, token] (uapmd::DocumentIOResult ioResult, std::vector<uint8_t> data)
        {
            if (! ioResult.success)
            {
                // Remove entry whose read failed
                std::erase_if (fileHistory_,
                    [&token] (const ScriptFileEntry& e) { return e.token == token; });
                saveSettings();
                errorMessage_ = "Failed to read file: " + ioResult.error;
                return;
            }

            auto len = std::min (data.size(), scriptBuffer_.size() - 1);
            std::memcpy (scriptBuffer_.data(), data.data(), len);
            scriptBuffer_[len] = '\0';
            errorMessage_.clear();
        });
}

void ScriptEditor::addToHistory (const std::string& token, const std::string& displayName)
{
    // Deduplicate — remove any existing entry with the same token
    std::erase_if (fileHistory_,
        [&token] (const ScriptFileEntry& e) { return e.token == token; });

    // Insert at the front (most-recently-used ordering)
    fileHistory_.insert (fileHistory_.begin(), {token, displayName});

    // Cap at 20 entries
    if (fileHistory_.size() > 20)
        fileHistory_.resize (20);

    saveSettings();
}

// ── Settings persistence (desktop only) ──────────────────────────────────────

std::string ScriptEditor::getSettingsFilePath()
{
#if !ANDROID && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
    auto dir = cpplocate::localDir ("uapmd-app");
    if (dir.empty())
        return {};
    auto dirPath = std::filesystem::path{dir};
    std::error_code ec;
    std::filesystem::create_directories (dirPath, ec);
    return (dirPath / "settings.json").string();
#else
    return {};
#endif
}

void ScriptEditor::loadSettings()
{
    auto path = getSettingsFilePath();
    if (path.empty())
        return;

    std::ifstream f (path);
    if (! f.is_open())
        return;

    std::stringstream ss;
    ss << f.rdbuf();

    try
    {
        auto root = choc::json::parse (ss.str());
        auto historyView = root["scriptFileHistory"];
        if (! historyView.isArray())
            return;

        for (uint32_t i = 0; i < historyView.size() && fileHistory_.size() < 20; ++i)
        {
            auto entry = historyView[i];
            if (! entry.isObject())
                continue;
            auto tokenVal = entry["token"];
            auto displayVal = entry["displayName"];
            if (! tokenVal.isString() || ! displayVal.isString())
                continue;
            auto token = std::string (tokenVal.getString());
            if (token.empty())
                continue;
            fileHistory_.push_back ({token, std::string (displayVal.getString())});
        }
    }
    catch (...) {}
}

void ScriptEditor::saveSettings()
{
    auto path = getSettingsFilePath();
    if (path.empty())
        return;

    auto arr = choc::value::createEmptyArray();
    for (auto& entry : fileHistory_)
        arr.addArrayElement (choc::value::createObject (
            "HistoryEntry",
            "token",       entry.token,
            "displayName", entry.displayName));

    auto root = choc::value::createObject (
        "ScriptEditorSettings",
        "scriptFileHistory", arr);

    std::ofstream f (path);
    if (f.is_open())
        f << choc::json::toString (root, true);
}

std::string ScriptEditor::getJsLibraryPath (const std::string& modulePath) const
{
    // Remove leading './' if present
    std::string cleanPath = modulePath;
    if (cleanPath.starts_with ("./"))
        cleanPath = cleanPath.substr (2);
    else if (cleanPath.starts_with ("/"))
        cleanPath = cleanPath.substr (1);

#if ANDROID || (defined(__APPLE__) && TARGET_OS_IPHONE)
    // No cpplocate on mobile platforms; JS module resolution is not supported here.
    std::filesystem::path exeDir{};
#elif defined(__EMSCRIPTEN__)
    std::filesystem::path exeDir{};
#else
    // Use cpplocate to find the executable directory
    std::filesystem::path exeDir = cpplocate::getExecutablePath();
    if (! exeDir.empty())
        exeDir = exeDir.parent_path();
    else
        exeDir = std::filesystem::current_path();  // Fallback
#endif

    // Try different base paths for module resolution
    std::vector<std::filesystem::path> basePaths = {
        exeDir / "js" / "src",                              // Next to executable (symlinked js/src)
        exeDir / "js" / "dist" / "src",                     // Next to executable (compiled)
        exeDir.parent_path() / "js" / "src",                // One level up
        exeDir.parent_path() / "js" / "dist" / "src",       // One level up (compiled)
        exeDir.parent_path().parent_path() / "js" / "src",  // Two levels up (for build dirs)
        exeDir.parent_path().parent_path() / "js" / "dist" / "src"  // Two levels up (compiled)
    };

    for (const auto& basePath : basePaths)
    {
        auto fullPath = basePath / cleanPath;

        if (std::filesystem::exists (fullPath))
            return fullPath.string();

        // Also try with .js extension if not already present
        if (! cleanPath.ends_with (".js"))
        {
            auto pathWithExt = basePath / (cleanPath + ".js");
            if (std::filesystem::exists (pathWithExt))
                return pathWithExt.string();
        }
    }

    return "";
}

} // namespace uapmd::gui
