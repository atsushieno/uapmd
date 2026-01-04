#include "ScriptEditor.hpp"
#include <imgui.h>
#include <choc/javascript/choc_javascript.h>
#include <choc/javascript/choc_javascript_QuickJS.h>
#include <cpplocate/cpplocate.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include "../AppModel.hpp"

namespace uapmd::gui {

ScriptEditor::ScriptEditor()
{
    // Create JavaScript language definition
    static TextEditor::LanguageDefinition langDef;
    static bool initialized = false;

    if (!initialized)
    {
        langDef.mName = "JavaScript";
        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = true;
        langDef.mSingleLineComment = "//";
        langDef.mCommentStart = "/*";
        langDef.mCommentEnd = "*/";

    // JavaScript keywords
    static const char* const keywords[] = {
        "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
        "default", "delete", "do", "else", "export", "extends", "finally", "for", "from",
        "function", "if", "import", "in", "instanceof", "let", "new", "of", "return",
        "static", "super", "switch", "this", "throw", "try", "typeof", "var", "void",
        "while", "with", "yield", "true", "false", "null", "undefined"
    };
    for (auto& k : keywords)
        langDef.mKeywords.insert(k);

    // API type identifiers with their declarations
    static const char* const apiTypes[][2] = {
        {"PluginScanTool", "class PluginScanTool - Audio plugin scanning and catalog management"},
        {"PluginCatalog", "class PluginCatalog - Plugin catalog for storing scanned plugins"},
        {"PluginCatalogEntry", "class PluginCatalogEntry - Single plugin entry in catalog"},
        {"PluginFormat", "class PluginFormat - Plugin format descriptor (VST3, CLAP, AU, LV2)"},
        {"PluginInstance", "class PluginInstance - Instantiated plugin for audio processing"},
        {"ParameterInfo", "class ParameterInfo - Plugin parameter metadata"},
    };
    for (auto& [name, decl] : apiTypes)
        langDef.mIdentifiers.insert(std::make_pair(name, TextEditor::Identifier{{0, 0}, decl}));

    // PluginScanTool members
    static const char* const scanToolMembers[][2] = {
        {"catalog", "property: PluginCatalog - Access the plugin catalog"},
        {"performScanning", "method() - Scan for plugins in standard locations"},
        {"getFormats", "method(): PluginFormat[] - Get available plugin formats"},
        {"saveCache", "method(path: string) - Save catalog cache to file"},
        {"setCacheFile", "method(path: string) - Set default cache file path"},
        {"filterByFormat", "method(entries, format): PluginCatalogEntry[] - Filter entries by format"},
    };
    for (auto& [name, decl] : scanToolMembers)
        langDef.mIdentifiers.insert(std::make_pair(name, TextEditor::Identifier{{0, 0}, decl}));

    // PluginCatalog members
    static const char* const catalogMembers[][2] = {
        {"count", "property: number - Number of plugins in catalog"},
        {"getPluginAt", "method(index: number): PluginCatalogEntry - Get plugin by index"},
        {"getPlugins", "method(): PluginCatalogEntry[] - Get all plugins"},
        {"load", "method(path: string) - Load catalog from file"},
        {"save", "method(path: string) - Save catalog to file"},
    };
    for (auto& [name, decl] : catalogMembers)
        langDef.mIdentifiers.insert(std::make_pair(name, TextEditor::Identifier{{0, 0}, decl}));

    // PluginInstance members
    static const char* const instanceMembers[][2] = {
        {"instanceId", "property: number - Unique instance identifier"},
        {"configure", "method(config): boolean - Configure the plugin instance"},
        {"startProcessing", "method() - Start audio processing"},
        {"stopProcessing", "method() - Stop audio processing"},
        {"getParameters", "method(): ParameterInfo[] - Get all parameter metadata"},
        {"getParameterValue", "method(id: number): number - Get parameter value"},
        {"setParameterValue", "method(id: number, value: number) - Set parameter value"},
        {"dispose", "method() - Dispose the plugin instance"},
    };
    for (auto& [name, decl] : instanceMembers)
        langDef.mIdentifiers.insert(std::make_pair(name, TextEditor::Identifier{{0, 0}, decl}));

    // PluginCatalogEntry members
    static const char* const entryMembers[][2] = {
        {"format", "property: string - Plugin format (VST3, CLAP, AU, LV2)"},
        {"pluginId", "property: string - Unique plugin identifier"},
        {"displayName", "property: string - Human-readable plugin name"},
        {"vendorName", "property: string - Plugin vendor/manufacturer"},
        {"productUrl", "property: string - Product website URL"},
        {"bundlePath", "property: string - File system path to plugin bundle"},
    };
    for (auto& [name, decl] : entryMembers)
        langDef.mIdentifiers.insert(std::make_pair(name, TextEditor::Identifier{{0, 0}, decl}));

    // ParameterInfo members
    static const char* const paramMembers[][2] = {
        {"id", "property: number - Parameter index/identifier"},
        {"name", "property: string - Parameter display name"},
        {"minValue", "property: number - Minimum parameter value"},
        {"maxValue", "property: number - Maximum parameter value"},
        {"defaultValue", "property: number - Default parameter value"},
        {"isAutomatable", "property: boolean - Can be automated"},
        {"isReadonly", "property: boolean - Read-only parameter"},
    };
    for (auto& [name, decl] : paramMembers)
        langDef.mIdentifiers.insert(std::make_pair(name, TextEditor::Identifier{{0, 0}, decl}));

    // Common JavaScript globals
    static const char* const jsGlobals[][2] = {
        {"console", "object - Console logging API"},
        {"Array", "class Array - JavaScript array type"},
        {"Object", "class Object - JavaScript object type"},
        {"String", "class String - JavaScript string type"},
        {"Number", "class Number - JavaScript number type"},
        {"Boolean", "class Boolean - JavaScript boolean type"},
        {"JSON", "object - JSON parse/stringify utilities"},
        {"Math", "object - Mathematical functions"},
        {"Date", "class Date - Date and time utilities"},
        {"Promise", "class Promise - Asynchronous operation"},
    };
    for (auto& [name, decl] : jsGlobals)
        langDef.mIdentifiers.insert(std::make_pair(name, TextEditor::Identifier{{0, 0}, decl}));

        // Token regex patterns for syntax highlighting
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
            "\\\"(\\\\.|[^\\\"])*\\\"", TextEditor::PaletteIndex::String));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
            "\\'(\\\\.|[^\\'])*\\'", TextEditor::PaletteIndex::String));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
            "`(\\\\.|[^`])*`", TextEditor::PaletteIndex::String));  // Template literals
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
            "[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?", TextEditor::PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
            "0[xX][0-9a-fA-F]+", TextEditor::PaletteIndex::Number));  // Hex numbers
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
            "[a-zA-Z_$][a-zA-Z0-9_$]*", TextEditor::PaletteIndex::Identifier));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
            "[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", TextEditor::PaletteIndex::Punctuation));

        initialized = true;
    }

    editor_.SetLanguageDefinition(langDef);
    editor_.SetShowWhitespaces (false);
    setDefaultScript();
    initializeJavaScriptContext();
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

    ImGui::SetNextWindowSize (ImVec2 (800, 600), ImGuiCond_FirstUseEver);

    if (ImGui::Begin ("Script Editor", &isOpen_))
    {
        // Render the text editor
        editor_.Render ("ScriptTextEditor");

        ImGui::Separator();

        // Run button
        if (ImGui::Button ("Run"))
        {
            executeScript();
        }

        ImGui::SameLine();

        // Reset button
        if (ImGui::Button ("Reset to Default"))
        {
            setDefaultScript();
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

void ScriptEditor::initializeJavaScriptContext()
{
    jsContext_ = choc::javascript::createQuickJSContext();

    // Register console.log for debugging
    jsContext_.registerFunction ("log", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        std::string output;

        for (size_t i = 0; i < args.numArgs; ++i)
        {
            if (i > 0)
                output += " ";

            auto* arg = args[i];

            if (arg != nullptr)
                output += choc::json::toString (*arg, true);
        }

        // Output to stdout for now (could be directed to an ImGui log window)
        std::cout << "[JS] " << output << std::endl;
        return choc::value::Value();
    });

    // === Plugin Catalog API ===

    jsContext_.registerFunction ("__remidy_catalog_get_count", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto& catalog = sequencer.catalog();
        auto plugins = catalog.getPlugins();
        return choc::value::createInt32 (static_cast<int32_t>(plugins.size()));
    });

    jsContext_.registerFunction ("__remidy_catalog_get_plugin_at", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto index = args.get<int32_t> (0, -1);
        if (index < 0)
            return choc::value::Value();

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto& catalog = sequencer.catalog();
        auto plugins = catalog.getPlugins();

        if (index >= static_cast<int32_t>(plugins.size()))
            return choc::value::Value();

        auto* entry = plugins[static_cast<size_t>(index)];
        if (! entry)
            return choc::value::Value();

        auto obj = choc::value::createObject ("PluginCatalogEntry");
        obj.setMember ("format", entry->format());
        obj.setMember ("pluginId", entry->pluginId());
        obj.setMember ("displayName", entry->displayName());
        obj.setMember ("vendorName", entry->vendorName());
        obj.setMember ("productUrl", entry->productUrl());
        obj.setMember ("bundlePath", entry->bundlePath().string());
        return obj;
    });

    jsContext_.registerFunction ("__remidy_catalog_load", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (pathStr.empty())
            return choc::value::createBool (false);

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto& catalog = sequencer.catalog();
        std::filesystem::path path (pathStr);
        catalog.load (path);
        return choc::value::createBool (true);
    });

    jsContext_.registerFunction ("__remidy_catalog_save", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (pathStr.empty())
            return choc::value::createBool (false);

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto& catalog = sequencer.catalog();
        std::filesystem::path path (pathStr);
        catalog.save (path);
        return choc::value::createBool (true);
    });

    // === Plugin Scan Tool API ===

    jsContext_.registerFunction ("__remidy_scan_tool_perform_scanning", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        uapmd::AppModel::instance().performPluginScanning (false);
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_scan_tool_get_formats", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& scanTool = uapmd::AppModel::instance().pluginScanTool();
        auto formats = scanTool.formats();

        auto arr = choc::value::createEmptyArray();
        for (auto* format : formats)
        {
            arr.addArrayElement (choc::value::createString (format->name()));
        }
        return arr;
    });

    jsContext_.registerFunction ("__remidy_scan_tool_save_cache", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (! pathStr.empty())
        {
            auto& scanTool = uapmd::AppModel::instance().pluginScanTool();
            std::filesystem::path path (pathStr);
            scanTool.savePluginListCache (path);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_scan_tool_set_cache_file", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (! pathStr.empty())
        {
            auto& scanTool = uapmd::AppModel::instance().pluginScanTool();
            scanTool.pluginListCacheFile() = std::filesystem::path (pathStr);
        }
        return choc::value::Value();
    });

    // === Plugin Instance API ===

    jsContext_.registerFunction ("__remidy_instance_create", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto formatName = args.get<std::string> (0, "");
        auto pluginId = args.get<std::string> (1, "");

        if (formatName.empty() || pluginId.empty())
            return choc::value::createInt32 (-1);

        // Use a simple synchronous approach for now
        // In a real implementation, you might want to handle the async callback properly
        std::atomic<int32_t> resultInstanceId{-1};
        std::atomic<bool> completed{false};

        uapmd::AppModel::instance().instantiatePlugin (0, formatName, pluginId);

        // For now, return a placeholder. The actual instance ID will be available via callback
        // This is a limitation of the synchronous bridge - ideally we'd use promises/async
        return choc::value::createInt32 (0);  // Temporary placeholder
    });

    jsContext_.registerFunction ("__remidy_instance_get_parameters", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createEmptyArray();

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto* instance = sequencer.getPluginInstance (instanceId);

        if (! instance)
            return choc::value::createEmptyArray();

        auto parameters = instance->parameterMetadataList();
        auto arr = choc::value::createEmptyArray();

        for (const auto& param : parameters)
        {
            auto obj = choc::value::createObject ("ParameterInfo");
            obj.setMember ("id", static_cast<int32_t>(param.index));
            obj.setMember ("name", param.name);
            obj.setMember ("minValue", param.minPlainValue);
            obj.setMember ("maxValue", param.maxPlainValue);
            obj.setMember ("defaultValue", param.defaultPlainValue);
            obj.setMember ("isAutomatable", param.automatable);
            obj.setMember ("isReadonly", false); // Assuming all are writable for now
            arr.addArrayElement (obj);
        }

        return arr;
    });

    jsContext_.registerFunction ("__remidy_instance_get_parameter_value", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto paramId = args.get<int32_t> (1, -1);

        if (instanceId < 0 || paramId < 0)
            return choc::value::createFloat64 (0.0);

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto* instance = sequencer.getPluginInstance (instanceId);

        if (! instance)
            return choc::value::createFloat64 (0.0);

        auto value = instance->getParameterValue (paramId);
        return choc::value::createFloat64 (value);
    });

    jsContext_.registerFunction ("__remidy_instance_set_parameter_value", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto paramId = args.get<int32_t> (1, -1);
        auto value = args.get<double> (2, 0.0);

        if (instanceId < 0 || paramId < 0)
            return choc::value::Value();

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto* instance = sequencer.getPluginInstance (instanceId);

        if (instance)
        {
            instance->setParameterValue (paramId, value);
        }

        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_dispose", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId >= 0)
        {
            uapmd::AppModel::instance().removePluginInstance (instanceId);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_configure", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        // Placeholder for configuration
        return choc::value::createBool (true);
    });

    jsContext_.registerFunction ("__remidy_instance_start_processing", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        // Placeholder
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_stop_processing", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        // Placeholder
        return choc::value::Value();
    });
}

void ScriptEditor::executeScript()
{
    errorMessage_.clear();

    try
    {
        auto scriptText = editor_.GetText();

        if (! jsContext_)
        {
            initializeJavaScriptContext();
        }

        // Check if the script contains import statements (for module support)
        bool hasImports = scriptText.find ("import") != std::string::npos;

        if (hasImports)
        {
            // Use runModule for scripts with imports
            bool completed = false;
            std::string lastError;
            choc::value::Value lastResult;

            jsContext_.runModule (scriptText,
                // Module resolver function
                [this] (std::string_view modulePath) -> std::optional<std::string>
                {
                    auto fullPath = getJsLibraryPath (std::string (modulePath));

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
            auto result = jsContext_.evaluateExpression (scriptText);

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
    std::string defaultScript = R"(// JavaScript Evaluation Engine for UAPMD
// Import the remidy bridge module for plugin access
import { PluginScanTool, PluginInstance } from 'remidy-bridge';

// Example 1: Scan for plugins and list them
log('Creating scan tool...');
const scanTool = new PluginScanTool();

log('Scanning for plugins...');
scanTool.performScanning();

const catalog = scanTool.catalog;
log('Found', catalog.count, 'plugins');

// List first 5 plugins
const plugins = catalog.getPlugins();
for (let i = 0; i < Math.min(5, plugins.length); i++) {
    const plugin = plugins[i];
    log(`${i + 1}. ${plugin.displayName} (${plugin.format}) by ${plugin.vendorName}`);
}

// Example 2: Save catalog to cache
// scanTool.saveCache('plugin-cache.json');

// Example 3: Load a plugin instance
// const formats = scanTool.getFormats();
// if (formats.length > 0 && plugins.length > 0) {
//     const instance = new PluginInstance(plugins[0].format, plugins[0].pluginId);
//     log('Created instance:', instance.instanceId);
//
//     const params = instance.getParameters();
//     log('Plugin has', params.length, 'parameters');
//
//     instance.dispose();
// }

)";

    editor_.SetText (defaultScript);
}

std::string ScriptEditor::getJsLibraryPath (const std::string& modulePath) const
{
    // Remove leading './' if present
    std::string cleanPath = modulePath;
    if (cleanPath.starts_with ("./"))
        cleanPath = cleanPath.substr (2);
    else if (cleanPath.starts_with ("/"))
        cleanPath = cleanPath.substr (1);

    // Use cpplocate to find the executable directory
    std::filesystem::path exeDir = cpplocate::getExecutablePath();
    if (! exeDir.empty())
        exeDir = exeDir.parent_path();
    else
        exeDir = std::filesystem::current_path();  // Fallback

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
