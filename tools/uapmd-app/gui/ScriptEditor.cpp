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
#include <cstring>
#include "../AppModel.hpp"

namespace uapmd::gui {

ScriptEditor::ScriptEditor()
{
    // Initialize buffer with a large size for the text editor
    scriptBuffer_.resize(65536, '\0');
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
        // Calculate available height for the text editor, leaving space for buttons and error messages
        float buttonHeight = ImGui::GetFrameHeightWithSpacing() * 2;  // Space for buttons
        float errorHeight = errorMessage_.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * 3;  // Space for error message
        float availableHeight = ImGui::GetContentRegionAvail().y - buttonHeight - errorHeight;

        // Render the text editor with constrained height using ImGui's built-in multiline input
        ImGui::InputTextMultiline("##ScriptEditor", scriptBuffer_.data(), scriptBuffer_.size(),
                                   ImVec2(0, availableHeight),
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

        // Use the unified AppModel API which calls global callbacks
        // However, JavaScript needs synchronous return, so we still need to wait
        auto& model = uapmd::AppModel::instance();

        uapmd::AppModel::PluginInstanceConfig config;
        // config uses defaults: apiName="default", auto-generated device name, etc.

        std::atomic<int32_t> resultInstanceId{-1};
        std::atomic<bool> completed{false};

        // Register a temporary callback to capture the result for this specific call
        auto callback = [&resultInstanceId, &completed](const uapmd::AppModel::PluginInstanceResult& result) {
            if (!result.error.empty() || result.instanceId < 0) {
                std::cerr << "[JS] Failed to create plugin instance: " << result.error << std::endl;
                resultInstanceId = -1;
            } else {
                resultInstanceId = result.instanceId;
            }
            completed = true;
        };

        // Temporarily add our callback
        model.instanceCreated.push_back(callback);

        // Trigger instance creation (will call all callbacks including ours and MainWindow's)
        model.createPluginInstanceAsync(formatName, pluginId, -1, config);

        // Wait for completion with timeout (max 5 seconds)
        auto startTime = std::chrono::steady_clock::now();
        while (!completed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (elapsed > std::chrono::seconds(5)) {
                std::cerr << "[JS] Timeout creating plugin instance" << std::endl;
                // Remove our callback
                model.instanceCreated.pop_back();
                return choc::value::createInt32 (-1);
            }
        }

        // Remove our temporary callback
        model.instanceCreated.pop_back();

        return choc::value::createInt32 (resultInstanceId.load());
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

    // === Sequencer MIDI Control API ===

    jsContext_.registerFunction ("__remidy_sequencer_sendNoteOn", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto note = args.get<int32_t> (1, 60);

        if (instanceId >= 0 && note >= 0 && note < 128)
        {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            sequencer.sendNoteOn (instanceId, note);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_sendNoteOff", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto note = args.get<int32_t> (1, 60);

        if (instanceId >= 0 && note >= 0 && note < 128)
        {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            sequencer.sendNoteOff (instanceId, note);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_setParameterValue", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto paramIndex = args.get<int32_t> (1, -1);
        auto value = args.get<double> (2, 0.0);

        if (instanceId >= 0 && paramIndex >= 0)
        {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            sequencer.setParameterValue (instanceId, paramIndex, value);
        }
        return choc::value::Value();
    });

    // === Sequencer Transport/Playback API ===

    jsContext_.registerFunction ("__remidy_sequencer_startPlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.startPlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_stopPlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.stopPlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_pausePlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.pausePlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_resumePlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.resumePlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_getPlaybackPosition", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto position = sequencer.playbackPositionSamples();
        return choc::value::createInt64 (position);
    });

    // === Sequencer Instance Management API ===

    jsContext_.registerFunction ("__remidy_sequencer_getInstanceIds", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto instanceIds = sequencer.getInstanceIds();

        auto arr = choc::value::createEmptyArray();
        for (auto id : instanceIds)
        {
            arr.addArrayElement (choc::value::createInt32 (id));
        }
        return arr;
    });

    jsContext_.registerFunction ("__remidy_sequencer_getPluginName", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createString ("");

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto name = sequencer.getPluginName (instanceId);
        return choc::value::createString (name);
    });

    jsContext_.registerFunction ("__remidy_sequencer_getPluginFormat", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createString ("");

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto format = sequencer.getPluginFormat (instanceId);
        return choc::value::createString (format);
    });

    jsContext_.registerFunction ("__remidy_sequencer_isPluginBypassed", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createBool (false);

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto bypassed = sequencer.isPluginBypassed (instanceId);
        return choc::value::createBool (bypassed);
    });

    jsContext_.registerFunction ("__remidy_sequencer_setPluginBypassed", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto bypassed = args.get<bool> (1, false);

        if (instanceId >= 0)
        {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            sequencer.setPluginBypassed (instanceId, bypassed);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_getTrackInfos", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto trackInfos = sequencer.getTrackInfos();

        auto arr = choc::value::createEmptyArray();
        for (const auto& track : trackInfos)
        {
            auto trackObj = choc::value::createObject ("TrackInfo");
            trackObj.setMember ("trackIndex", track.trackIndex);

            auto nodesArr = choc::value::createEmptyArray();
            for (const auto& node : track.nodes)
            {
                auto nodeObj = choc::value::createObject ("PluginNodeInfo");
                nodeObj.setMember ("instanceId", node.instanceId);
                nodeObj.setMember ("pluginId", node.pluginId);
                nodeObj.setMember ("format", node.format);
                nodeObj.setMember ("displayName", node.displayName);
                nodesArr.addArrayElement (nodeObj);
            }
            trackObj.setMember ("nodes", nodesArr);
            arr.addArrayElement (trackObj);
        }
        return arr;
    });

    jsContext_.registerFunction ("__remidy_sequencer_getParameterUpdates", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createEmptyArray();

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto updates = sequencer.getParameterUpdates (instanceId);

        auto arr = choc::value::createEmptyArray();
        for (const auto& update : updates)
        {
            auto obj = choc::value::createObject ("ParameterUpdate");
            obj.setMember ("parameterIndex", update.parameterIndex);
            obj.setMember ("value", update.value);
            arr.addArrayElement (obj);
        }
        return arr;
    });

    // === Sequencer Audio Analysis API ===

    jsContext_.registerFunction ("__remidy_sequencer_getInputSpectrum", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto numBars = args.get<int32_t> (0, 32);
        if (numBars <= 0 || numBars > 256)
            numBars = 32;

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        std::vector<float> spectrum (static_cast<size_t>(numBars));
        sequencer.getInputSpectrum (spectrum.data(), numBars);

        auto arr = choc::value::createEmptyArray();
        for (auto value : spectrum)
        {
            arr.addArrayElement (choc::value::createFloat32 (value));
        }
        return arr;
    });

    jsContext_.registerFunction ("__remidy_sequencer_getOutputSpectrum", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto numBars = args.get<int32_t> (0, 32);
        if (numBars <= 0 || numBars > 256)
            numBars = 32;

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        std::vector<float> spectrum (static_cast<size_t>(numBars));
        sequencer.getOutputSpectrum (spectrum.data(), numBars);

        auto arr = choc::value::createEmptyArray();
        for (auto value : spectrum)
        {
            arr.addArrayElement (choc::value::createFloat32 (value));
        }
        return arr;
    });

    // === Sequencer Audio Device/Settings API ===

    jsContext_.registerFunction ("__remidy_sequencer_getSampleRate", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto sampleRate = sequencer.sampleRate();
        return choc::value::createInt32 (sampleRate);
    });

    jsContext_.registerFunction ("__remidy_sequencer_setSampleRate", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto sampleRate = args.get<int32_t> (0, 44100);
        if (sampleRate <= 0)
            return choc::value::createBool (false);

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto success = sequencer.sampleRate (sampleRate);
        return choc::value::createBool (success);
    });

    jsContext_.registerFunction ("__remidy_sequencer_isScanning", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto isScanning = uapmd::AppModel::instance().isScanning();
        return choc::value::createBool (isScanning);
    });
}

void ScriptEditor::executeScript()
{
    errorMessage_.clear();

    try
    {
        auto scriptText = std::string(scriptBuffer_.data());

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
import { PluginScanTool, sequencer } from 'remidy-bridge';

// Example: Create tracks for VST3 Dexed, LV2 RipplerX, CLAP Six Sines, and AU Surge XT
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
    { name: 'ripplerx', format: 'LV2' },
    { name: 'six sines', format: 'CLAP' },
    { name: 'surge xt', format: 'AU' }
];

const instanceIds = [];

for (const trackConfig of tracksToCreate) {
    const plugin = findPlugin(trackConfig.name, trackConfig.format);

    if (plugin) {
        // Use the low-level sequencer API directly, matching the C++ AppModel::instantiatePlugin pattern
        const instanceId = sequencer.createPluginInstance(plugin.format, plugin.pluginId);
        if (instanceId >= 0) {
            instanceIds.push(instanceId);
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
