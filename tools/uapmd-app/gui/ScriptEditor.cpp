#include "ScriptEditor.hpp"
#include <imgui.h>
#if !ANDROID
#include <cpplocate/cpplocate.h>
#endif
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>

namespace uapmd::gui {

ScriptEditor::ScriptEditor()
{
    // Initialize buffer with a large size for the text editor
    scriptBuffer_.resize(65536, '\0');
    setDefaultScript();
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
    std::string defaultScript = R"(// JavaScript Evaluation Engine for UAPMD
// Import the remidy bridge module for plugin access
import { PluginScanTool, sequencer } from 'remidy-bridge';

// The public API is available via the global 'uapmd' object:
//   uapmd.catalog.*      - Plugin discovery and management
//   uapmd.scanTool.*     - Plugin scanning and caching
//   uapmd.instancing.*   - Plugin instance creation
//   uapmd.instance(id)   - Get PluginInstance wrapper (getParameters, setParameterValue, etc.)
//   uapmd.sequencer.*    - Audio engine, MIDI, and transport
//
// You can use either the OO wrapper (PluginScanTool, sequencer) or
// the direct uapmd.* API. Both approaches work!
//
// Example using uapmd API:
//   const id = uapmd.instancing.create("VST3", pluginId);
//   const instance = uapmd.instance(id);
//   instance.showUI();
//   instance.setParameterValue(0, 0.5);

// Example: Create tracks for VST3 Dexed, LV2 RipplerX, CLAP Six Sines, AU Surge XT, and AUv3 Mela FX
const scanTool = new PluginScanTool();
const catalog = scanTool.catalog;

// Helper function to find a plugin by name and format
// First tries exact match (case-insensitive), then falls back to includes()
function findPlugin(displayName, format) {
    const plugins = catalog.getPlugins();
    const lowerName = displayName.toLowerCase();

    // Try exact match first
    let plugin = plugins.find(p =>
        p.displayName.toLowerCase() === lowerName &&
        p.format === format
    );

    // Fall back to includes() if exact match not found
    if (!plugin) {
        plugin = plugins.find(p =>
            p.displayName.toLowerCase().includes(lowerName) &&
            p.format === format
        );
    }

    return plugin;
}

// Create four tracks with specific plugins
const tracksToCreate = [
    { name: 'Dexed', format: 'VST3' },
    { name: 'RipplerX', format: 'LV2' },
    { name: 'OctaSine', format: 'CLAP' },
    { name: 'Surge XT', format: 'AU' },
    { name: 'Mela FX', format: 'AU' }
];

const instanceIds = [];

for (const trackConfig of tracksToCreate) {
    const plugin = findPlugin(trackConfig.name, trackConfig.format);

    if (plugin) {
        // Use the low-level sequencer API directly, matching the C++ AppModel::instantiatePlugin pattern
        const instanceId = sequencer.createPluginInstance(plugin.format, plugin.pluginId);
        if (instanceId >= 0) {
            instanceIds.push(instanceId);
            sequencer.showPluginUI(instanceId);
        } else {
            log(`Failed to create ${plugin.displayName}`);
        }
    } else {
        log(`Plugin not found: ${trackConfig.name} (${trackConfig.format})`);
    }
}

// List all active tracks
const tracks = sequencer.getTrackInfos();
log(`Created ${instanceIds.length} track(s):`);
for (const track of tracks) {
    for (const node of track.nodes) {
        log(`  Track ${track.trackIndex}: ${node.displayName} (${node.format})`);
    }
}

// Example: Send MIDI notes to all created instances
// Uncomment to test MIDI functionality
// log('\nSending test MIDI notes to all instances...');
// for (const instanceId of instanceIds) {
//     sequencer.sendNoteOn(instanceId, 60);  // C4
//     sequencer.sendNoteOn(instanceId, 64);  // E4
//     sequencer.sendNoteOn(instanceId, 67);  // G4
// }

)";

    // Copy default script to buffer
    size_t len = std::min(defaultScript.length(), scriptBuffer_.size() - 1);
    std::memcpy(scriptBuffer_.data(), defaultScript.c_str(), len);
    scriptBuffer_[len] = '\0';
}

std::string ScriptEditor::getJsLibraryPath (const std::string& modulePath) const
{
    // Remove leading './' if present
    std::string cleanPath = modulePath;
    if (cleanPath.starts_with ("./"))
        cleanPath = cleanPath.substr (2);
    else if (cleanPath.starts_with ("/"))
        cleanPath = cleanPath.substr (1);

#if ANDROID
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
