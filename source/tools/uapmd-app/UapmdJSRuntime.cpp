#include "UapmdJSRuntime.hpp"
#include "AppModel.hpp"
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/time.h>
#endif
#include <choc/javascript/choc_javascript_QuickJS.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <filesystem>

namespace uapmd {

UapmdJSRuntime::UapmdJSRuntime()
{
    reinitialize();
    registerAllParameterListeners();
    registerAllMetadataListeners();
}

void UapmdJSRuntime::reinitialize()
{
    jsContext_ = choc::javascript::createQuickJSContext();

    registerConsoleFunctions();
    registerProjectAPI();
    registerPluginCatalogAPI();
    registerPluginScanToolAPI();
    registerPluginInstanceAPI();
    registerSequencerMidiAPI();
    registerSequencerTransportAPI();
    registerSequencerInstanceAPI();
    registerSequencerAudioAnalysisAPI();
    registerSequencerAudioDeviceAPI();
    registerTimelineAPI();
    registerRenderAPI();
}

void UapmdJSRuntime::registerConsoleFunctions()
{
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
}

void UapmdJSRuntime::registerProjectAPI()
{
    jsContext_.registerFunction ("__remidy_project_save", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto filepath = args.get<std::string> (0, "");
        auto result = choc::value::createObject ("ProjectResult");
        if (filepath.empty())
        {
            result.setMember ("success", false);
            result.setMember ("error", "Project path is empty");
            return result;
        }

        auto projectResult = uapmd::AppModel::instance().saveProjectSync(filepath);
        result.setMember ("success", projectResult.success);
        result.setMember ("error", projectResult.error);
        return result;
    });

    jsContext_.registerFunction ("__remidy_project_load", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto filepath = args.get<std::string> (0, "");
        auto result = choc::value::createObject ("ProjectResult");
        if (filepath.empty())
        {
            result.setMember ("success", false);
            result.setMember ("error", "Project path is empty");
            return result;
        }

        auto projectResult = uapmd::AppModel::instance().loadProjectFromResolvedPath(filepath);
        result.setMember ("success", projectResult.success);
        result.setMember ("error", projectResult.error);
        return result;
    });
}

void UapmdJSRuntime::registerPluginCatalogAPI()
{
    jsContext_.registerFunction ("__remidy_catalog_get_count", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        const auto& sequencer = uapmd::AppModel::instance().sequencer();
        const auto plugins = sequencer.engine()->pluginHost()->pluginCatalogEntries();
        return choc::value::createInt32 (static_cast<int32_t>(plugins.size()));
    });

    jsContext_.registerFunction ("__remidy_catalog_get_plugin_at", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto index = args.get<int32_t> (0, -1);
        if (index < 0)
            return choc::value::Value();

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto plugins = sequencer.engine()->pluginHost()->pluginCatalogEntries();

        if (index >= static_cast<int32_t>(plugins.size()))
            return choc::value::Value();

        auto entry = plugins[static_cast<size_t>(index)];
        if (entry.pluginId().empty())
            return choc::value::Value();

        auto obj = choc::value::createObject ("PluginCatalogEntry");
        obj.setMember ("format", entry.format());
        obj.setMember ("pluginId", entry.pluginId());
        obj.setMember ("displayName", entry.displayName());
        obj.setMember ("vendorName", entry.vendorName());
        obj.setMember ("productUrl", entry.productUrl());
        obj.setMember ("bundlePath", entry.bundlePath().string());
        return obj;
    });

    jsContext_.registerFunction ("__remidy_catalog_save", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto pathStr = args.get<std::string> (0, "");
        if (pathStr.empty())
            return choc::value::createBool (false);

        std::filesystem::path path (pathStr);
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.engine()->pluginHost()->savePluginCatalogToFile(path);
        return choc::value::createBool (true);
    });
}

void UapmdJSRuntime::registerPluginScanToolAPI()
{
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
}

void UapmdJSRuntime::registerPluginInstanceAPI()
{
    jsContext_.registerFunction ("__remidy_instance_create", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto formatName = args.get<std::string> (0, "");
        auto pluginId = args.get<std::string> (1, "");
        auto trackIndex = args.get<int32_t> (2, -1);

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
        model.createPluginInstanceAsync(formatName, pluginId, trackIndex, config);

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
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

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
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

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
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

        if (instance)
        {
            // FIXME: it must not directly invoke this function from this non-RT-safe context.
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

    jsContext_.registerFunction ("__remidy_instance_enable_ump_device", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto deviceName = args.get<std::string> (1, "");

        if (instanceId >= 0)
        {
            uapmd::AppModel::instance().enableUmpDevice (instanceId, deviceName);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_disable_ump_device", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);

        if (instanceId >= 0)
        {
            uapmd::AppModel::instance().disableUmpDevice (instanceId);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_show_ui", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);

        if (instanceId >= 0)
        {
            uapmd::AppModel::instance().requestShowPluginUI (instanceId);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_hide_ui", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);

        if (instanceId >= 0)
        {
            uapmd::AppModel::instance().hidePluginUI (instanceId);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_show_details", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId >= 0)
            uapmd::AppModel::instance().requestShowInstanceDetails(instanceId);
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_instance_save_state", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto filepath = args.get<std::string> (1, "");

        if (instanceId < 0 || filepath.empty())
        {
            auto result = choc::value::createObject ("PluginStateResult");
            result.setMember ("success", false);
            result.setMember ("error", "Invalid instance ID or filepath");
            return result;
        }

        auto result = uapmd::AppModel::instance().savePluginStateSync (instanceId, filepath);

        auto obj = choc::value::createObject ("PluginStateResult");
        obj.setMember ("success", result.success);
        obj.setMember ("error", result.error);
        obj.setMember ("filepath", result.filepath);
        obj.setMember ("instanceId", result.instanceId);
        return obj;
    });

    jsContext_.registerFunction ("__remidy_instance_load_state", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto filepath = args.get<std::string> (1, "");

        if (instanceId < 0 || filepath.empty())
        {
            auto result = choc::value::createObject ("PluginStateResult");
            result.setMember ("success", false);
            result.setMember ("error", "Invalid instance ID or filepath");
            return result;
        }

        auto result = uapmd::AppModel::instance().loadPluginStateSync (instanceId, filepath);

        auto obj = choc::value::createObject ("PluginStateResult");
        obj.setMember ("success", result.success);
        obj.setMember ("error", result.error);
        obj.setMember ("filepath", result.filepath);
        obj.setMember ("instanceId", result.instanceId);
        return obj;
    });

    jsContext_.registerFunction ("__remidy_instance_get_presets", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createEmptyArray();

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

        if (! instance)
            return choc::value::createEmptyArray();

        auto presets = instance->presetMetadataList();
        auto arr = choc::value::createEmptyArray();

        for (const auto& p : presets)
        {
            auto obj = choc::value::createObject ("PresetInfo");
            obj.setMember ("index", static_cast<int32_t>(p.index));
            obj.setMember ("name", p.name);
            obj.setMember ("bank", static_cast<int32_t>(p.bank));
            arr.addArrayElement (obj);
        }

        return arr;
    });

    jsContext_.registerFunction ("__remidy_instance_load_preset", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto presetIndex = args.get<int32_t> (1, -1);

        if (instanceId < 0 || presetIndex < 0)
            return choc::value::createBool (false);

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);

        if (instance)
        {
            instance->loadPreset (presetIndex);
            return choc::value::createBool (true);
        }

        return choc::value::createBool (false);
    });
}

void UapmdJSRuntime::registerSequencerMidiAPI()
{
    jsContext_.registerFunction ("__remidy_sequencer_sendNoteOn", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto note = args.get<int32_t> (1, 60);

        if (instanceId >= 0 && note >= 0 && note < 128)
        {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            sequencer.engine()->sendNoteOn (instanceId, note);
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
            sequencer.engine()->sendNoteOff (instanceId, note);
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
            // FIXME: it must not invoke this function from this non-RT-safe context.
            sequencer.engine()->setParameterValue (instanceId, paramIndex, value);
        }
        return choc::value::Value();
    });
}

void UapmdJSRuntime::registerSequencerTransportAPI()
{
    jsContext_.registerFunction ("__remidy_sequencer_startPlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.engine()->startPlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_stopPlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.engine()->stopPlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_pausePlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.engine()->pausePlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_resumePlayback", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        sequencer.engine()->resumePlayback();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_getPlaybackPosition", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto position = sequencer.engine()->playbackPosition();
        return choc::value::createInt64 (position);
    });
}

void UapmdJSRuntime::registerSequencerInstanceAPI()
{
    jsContext_.registerFunction ("__remidy_sequencer_getInstanceIds", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto instanceIds = sequencer.engine()->pluginHost()->instanceIds();

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
        auto instance = sequencer.engine()->getPluginInstance (instanceId);
        return choc::value::createString (instance->displayName());
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
        auto* instance = sequencer.engine()->getPluginInstance (instanceId);
        return choc::value::createBool (instance ? instance->bypassed() : false);
    });

    jsContext_.registerFunction ("__remidy_sequencer_setPluginBypassed", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto bypassed = args.get<bool> (1, false);

        if (instanceId >= 0)
        {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            if (auto* instance = sequencer.engine()->getPluginInstance (instanceId))
                instance->bypassed (bypassed);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_getTrackInfos", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto tracks = sequencer.engine()->tracks();

        auto arr = choc::value::createEmptyArray();
        for (uapmd_track_index_t i = 0, n = tracks.size(); i < n; ++i) {
            auto track = tracks[i];
            auto trackObj = choc::value::createObject ("TrackInfo");
            trackObj.setMember ("trackIndex", i);

            auto nodesArr = choc::value::createEmptyArray();
            for (const int32_t instanceId : track->orderedInstanceIds()) {
                auto p = track->graph().getPluginNode(instanceId);
                if (!p)
                    continue;
                auto node = p->instance();
                auto nodeObj = choc::value::createObject ("PluginNodeInfo");
                nodeObj.setMember ("instanceId", instanceId);
                nodeObj.setMember ("pluginId", node->pluginId());
                nodeObj.setMember ("format", node->formatName());
                nodeObj.setMember ("displayName", node->displayName());
                nodesArr.addArrayElement (nodeObj);
            }
            trackObj.setMember ("nodes", nodesArr);
            arr.addArrayElement (trackObj);
        }
        return arr;
    });

    jsContext_.registerFunction ("__remidy_sequencer_add_track", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto index = uapmd::AppModel::instance().addTrack();
        return choc::value::createInt32(index);
    });

    jsContext_.registerFunction ("__remidy_sequencer_remove_track", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        bool removed = false;
        if (trackIndex >= 0) {
            removed = uapmd::AppModel::instance().removeTrack(trackIndex);
        }
        return choc::value::createBool(removed);
    });

    jsContext_.registerFunction ("__remidy_sequencer_clear_tracks", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        uapmd::AppModel::instance().removeAllTracks();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_getParameterUpdates", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        if (instanceId < 0)
            return choc::value::createEmptyArray();

        std::vector<ParameterUpdate> updates;
        {
            std::lock_guard<std::mutex> lock(js_parameter_mutex_);
            auto it = js_parameter_updates_.find(instanceId);
            if (it != js_parameter_updates_.end()) {
                updates = std::move(it->second);
                js_parameter_updates_.erase(it);
            }
        }

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

    jsContext_.registerFunction ("__remidy_sequencer_consumeParameterMetadataRefresh", [this] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        bool refreshed = false;
        if (instanceId >= 0)
        {
            std::lock_guard<std::mutex> lock(js_metadata_mutex_);
            auto it = js_metadata_refresh_.find(instanceId);
            if (it != js_metadata_refresh_.end()) {
                js_metadata_refresh_.erase(it);
                refreshed = true;
            }
        }
        return choc::value::createBool (refreshed);
    });
}

void UapmdJSRuntime::registerSequencerAudioAnalysisAPI()
{
    jsContext_.registerFunction ("__remidy_sequencer_getInputSpectrum", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto numBars = args.get<int32_t> (0, 32);
        if (numBars <= 0 || numBars > 256)
            numBars = 32;

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        std::vector<float> spectrum (static_cast<size_t>(numBars));
        sequencer.engine()->getInputSpectrum (spectrum.data(), numBars);

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
        sequencer.engine()->getOutputSpectrum (spectrum.data(), numBars);

        auto arr = choc::value::createEmptyArray();
        for (auto value : spectrum)
        {
            arr.addArrayElement (choc::value::createFloat32 (value));
        }
        return arr;
    });
}

void UapmdJSRuntime::registerSequencerAudioDeviceAPI()
{
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

void UapmdJSRuntime::registerParameterListener(int32_t instanceId)
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        auto node = track->graph().getPluginNode(instanceId);
        if (node) {
            auto listenerId = node->parameterUpdateEvent().addListener([this, instanceId](int32_t paramIndex, double value) {
                std::lock_guard<std::mutex> lock(js_parameter_mutex_);
                js_parameter_updates_[instanceId].push_back({paramIndex, value});
            });
            std::lock_guard<std::mutex> lock(js_parameter_mutex_);
            js_parameter_listener_ids_[instanceId] = listenerId;
            break;
        }
    }
}

void UapmdJSRuntime::unregisterParameterListener(int32_t instanceId)
{
    EventListenerId listenerId = 0;
    {
        std::lock_guard<std::mutex> lock(js_parameter_mutex_);
        auto it = js_parameter_listener_ids_.find(instanceId);
        if (it != js_parameter_listener_ids_.end()) {
            listenerId = it->second;
            js_parameter_listener_ids_.erase(it);
        }
        js_parameter_updates_.erase(instanceId);
    }

    if (listenerId != 0) {
        auto& seq = AppModel::instance().sequencer();
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterUpdateEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

void UapmdJSRuntime::registerAllParameterListeners()
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        for (const auto& [instanceId, node] : track->graph().plugins()) {
            if (node) {
                // Only register if not already registered
                std::lock_guard<std::mutex> lock(js_parameter_mutex_);
                if (js_parameter_listener_ids_.find(instanceId) == js_parameter_listener_ids_.end()) {
                    auto listenerId = node->parameterUpdateEvent().addListener([this, instanceId](int32_t paramIndex, double value) {
                        std::lock_guard<std::mutex> lock(js_parameter_mutex_);
                        js_parameter_updates_[instanceId].push_back({paramIndex, value});
                    });
                    js_parameter_listener_ids_[instanceId] = listenerId;
                }
            }
        }
    }
}

void UapmdJSRuntime::unregisterAllParameterListeners()
{
    std::map<int32_t, EventListenerId> listenerIdsCopy;
    {
        std::lock_guard<std::mutex> lock(js_parameter_mutex_);
        listenerIdsCopy = js_parameter_listener_ids_;
        js_parameter_listener_ids_.clear();
        js_parameter_updates_.clear();
    }

    auto& seq = AppModel::instance().sequencer();
    for (const auto& [instanceId, listenerId] : listenerIdsCopy) {
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterUpdateEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

void UapmdJSRuntime::registerMetadataListener(int32_t instanceId)
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        auto node = track->graph().getPluginNode(instanceId);
        if (node) {
            auto listenerId = node->parameterMetadataRefreshEvent().addListener([this, instanceId]() {
                std::lock_guard<std::mutex> lock(js_metadata_mutex_);
                js_metadata_refresh_.insert(instanceId);
            });
            std::lock_guard<std::mutex> lock(js_metadata_mutex_);
            js_metadata_listener_ids_[instanceId] = listenerId;
            break;
        }
    }
}

void UapmdJSRuntime::unregisterMetadataListener(int32_t instanceId)
{
    EventListenerId listenerId = 0;
    {
        std::lock_guard<std::mutex> lock(js_metadata_mutex_);
        auto it = js_metadata_listener_ids_.find(instanceId);
        if (it != js_metadata_listener_ids_.end()) {
            listenerId = it->second;
            js_metadata_listener_ids_.erase(it);
        }
        js_metadata_refresh_.erase(instanceId);
    }

    if (listenerId != 0) {
        auto& seq = AppModel::instance().sequencer();
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterMetadataRefreshEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

void UapmdJSRuntime::registerAllMetadataListeners()
{
    auto& seq = AppModel::instance().sequencer();
    for (const auto& track : seq.engine()->tracks()) {
        for (const auto& [instanceId, node] : track->graph().plugins()) {
            if (node) {
                // Only register if not already registered
                std::lock_guard<std::mutex> lock(js_metadata_mutex_);
                if (js_metadata_listener_ids_.find(instanceId) == js_metadata_listener_ids_.end()) {
                    auto listenerId = node->parameterMetadataRefreshEvent().addListener([this, instanceId]() {
                        std::lock_guard<std::mutex> lock(js_metadata_mutex_);
                        js_metadata_refresh_.insert(instanceId);
                    });
                    js_metadata_listener_ids_[instanceId] = listenerId;
                }
            }
        }
    }
}

void UapmdJSRuntime::unregisterAllMetadataListeners()
{
    std::map<int32_t, EventListenerId> listenerIdsCopy;
    {
        std::lock_guard<std::mutex> lock(js_metadata_mutex_);
        listenerIdsCopy = js_metadata_listener_ids_;
        js_metadata_listener_ids_.clear();
        js_metadata_refresh_.clear();
    }

    auto& seq = AppModel::instance().sequencer();
    for (const auto& [instanceId, listenerId] : listenerIdsCopy) {
        for (const auto& track : seq.engine()->tracks()) {
            auto node = track->graph().getPluginNode(instanceId);
            if (node) {
                node->parameterMetadataRefreshEvent().removeListener(listenerId);
                break;
            }
        }
    }
}

namespace {

constexpr std::string_view kMasterMarkerReferenceId = "master_track";

std::string timeReferenceTypeToString(uapmd::TimeReferenceType type) {
    switch (type) {
        case uapmd::TimeReferenceType::ContainerStart: return "containerStart";
        case uapmd::TimeReferenceType::ContainerEnd: return "containerEnd";
        case uapmd::TimeReferenceType::Point: return "point";
    }
    return "containerStart";
}

bool parseTimeReferenceType(std::string_view text, uapmd::TimeReferenceType& type) {
    if (text == "containerStart") {
        type = uapmd::TimeReferenceType::ContainerStart;
        return true;
    }
    if (text == "containerEnd") {
        type = uapmd::TimeReferenceType::ContainerEnd;
        return true;
    }
    if (text == "point") {
        type = uapmd::TimeReferenceType::Point;
        return true;
    }
    return false;
}

choc::value::Value serializeTimeReference(const uapmd::TimeReference& reference) {
    auto obj = choc::value::createObject("TimeReference");
    obj.setMember("type", timeReferenceTypeToString(reference.type));
    obj.setMember("referenceId", reference.referenceId);
    obj.setMember("offset", reference.offset);
    return obj;
}

choc::value::Value serializeMarker(const uapmd::ClipMarker& marker, std::string_view ownerReferenceId) {
    auto obj = choc::value::createObject("ClipMarker");
    obj.setMember("markerId", marker.markerId);
    obj.setMember("name", marker.name);
    obj.setMember("timeReference", serializeTimeReference(marker.timeReference(ownerReferenceId, kMasterMarkerReferenceId)));
    return obj;
}

choc::value::Value serializeWarp(const uapmd::AudioWarpPoint& warp, std::string_view ownerReferenceId) {
    auto obj = choc::value::createObject("AudioWarpPoint");
    obj.setMember("speedRatio", warp.speedRatio);
    obj.setMember("timeReference", serializeTimeReference(warp.timeReference(ownerReferenceId, kMasterMarkerReferenceId)));
    return obj;
}

bool parseTimeReferenceValue(const choc::value::ValueView& value, uapmd::TimeReference& reference, std::string& error) {
    if (!value.isObject()) {
        error = "timeReference must be an object";
        return false;
    }
    const auto typeText = value.hasObjectMember("type") ? value["type"].get<std::string>() : std::string{};
    if (!parseTimeReferenceType(typeText, reference.type)) {
        error = "timeReference.type is invalid";
        return false;
    }
    reference.referenceId = value.hasObjectMember("referenceId") ? value["referenceId"].get<std::string>() : std::string{};
    reference.offset = value.hasObjectMember("offset") ? value["offset"].get<double>() : 0.0;
    return true;
}

bool parseMarkersValue(const choc::value::ValueView* value,
                       std::string_view ownerReferenceId,
                       std::vector<uapmd::ClipMarker>& markers,
                       std::string& error) {
    markers.clear();
    if (!value)
        return true;
    if (!value->isArray()) {
        error = "markers must be an array";
        return false;
    }

    markers.reserve(value->size());
    for (uint32_t i = 0; i < value->size(); ++i) {
        const auto& item = (*value)[i];
        if (!item.isObject()) {
            error = "marker entry must be an object";
            return false;
        }
        uapmd::ClipMarker marker;
        marker.markerId = item.hasObjectMember("markerId") ? item["markerId"].get<std::string>() : std::string{};
        marker.name = item.hasObjectMember("name") ? item["name"].get<std::string>() : std::string{};
        uapmd::TimeReference reference;
        if (!item.hasObjectMember("timeReference") || !parseTimeReferenceValue(item["timeReference"], reference, error))
            return false;
        marker.setTimeReference(reference, ownerReferenceId, kMasterMarkerReferenceId);
        markers.push_back(std::move(marker));
    }
    return true;
}

bool parseWarpsValue(const choc::value::ValueView* value,
                     std::string_view ownerReferenceId,
                     std::vector<uapmd::AudioWarpPoint>& warps,
                     std::string& error) {
    warps.clear();
    if (!value)
        return true;
    if (!value->isArray()) {
        error = "audioWarps must be an array";
        return false;
    }

    warps.reserve(value->size());
    for (uint32_t i = 0; i < value->size(); ++i) {
        const auto& item = (*value)[i];
        if (!item.isObject()) {
            error = "audioWarp entry must be an object";
            return false;
        }
        uapmd::AudioWarpPoint warp;
        warp.speedRatio = item.hasObjectMember("speedRatio") ? item["speedRatio"].get<double>() : 1.0;
        uapmd::TimeReference reference;
        if (!item.hasObjectMember("timeReference") || !parseTimeReferenceValue(item["timeReference"], reference, error))
            return false;
        warp.setTimeReference(reference, ownerReferenceId, kMasterMarkerReferenceId);
        warps.push_back(std::move(warp));
    }
    return true;
}

std::string graphEndpointTypeToString(uapmd::AudioPluginGraphEndpointType type) {
    switch (type) {
        case uapmd::AudioPluginGraphEndpointType::GraphInput: return "graphInput";
        case uapmd::AudioPluginGraphEndpointType::GraphOutput: return "graphOutput";
        case uapmd::AudioPluginGraphEndpointType::Plugin: return "plugin";
    }
    return "plugin";
}

bool parseGraphEndpointType(std::string_view text, uapmd::AudioPluginGraphEndpointType& type) {
    if (text == "graphInput") {
        type = uapmd::AudioPluginGraphEndpointType::GraphInput;
        return true;
    }
    if (text == "graphOutput") {
        type = uapmd::AudioPluginGraphEndpointType::GraphOutput;
        return true;
    }
    if (text == "plugin") {
        type = uapmd::AudioPluginGraphEndpointType::Plugin;
        return true;
    }
    return false;
}

std::string graphBusTypeToString(uapmd::AudioPluginGraphBusType type) {
    return type == uapmd::AudioPluginGraphBusType::Event ? "event" : "audio";
}

bool parseGraphBusType(std::string_view text, uapmd::AudioPluginGraphBusType& type) {
    if (text == "audio") {
        type = uapmd::AudioPluginGraphBusType::Audio;
        return true;
    }
    if (text == "event") {
        type = uapmd::AudioPluginGraphBusType::Event;
        return true;
    }
    return false;
}

choc::value::Value serializeGraphEndpoint(const uapmd::AudioPluginGraphEndpoint& endpoint) {
    auto obj = choc::value::createObject("GraphEndpoint");
    obj.setMember("type", graphEndpointTypeToString(endpoint.type));
    obj.setMember("instanceId", endpoint.instance_id);
    obj.setMember("busIndex", static_cast<int32_t>(endpoint.bus_index));
    return obj;
}

choc::value::Value serializeGraphConnection(const uapmd::AudioPluginGraphConnection& connection) {
    auto obj = choc::value::createObject("GraphConnection");
    obj.setMember("id", connection.id);
    obj.setMember("busType", graphBusTypeToString(connection.bus_type));
    obj.setMember("source", serializeGraphEndpoint(connection.source));
    obj.setMember("target", serializeGraphEndpoint(connection.target));
    return obj;
}

} // namespace

void UapmdJSRuntime::registerTimelineAPI()
{
    jsContext_.registerFunction ("__remidy_timeline_get_state", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        const auto& state = uapmd::AppModel::instance().timeline();
        auto obj = choc::value::createObject ("TimelineState");
        obj.setMember ("tempo", state.tempo);
        obj.setMember ("timeSignatureNumerator", state.timeSignatureNumerator);
        obj.setMember ("timeSignatureDenominator", state.timeSignatureDenominator);
        obj.setMember ("isPlaying", state.isPlaying);
        obj.setMember ("playheadSamples", choc::value::createInt64 (state.playheadPosition.samples));
        obj.setMember ("loopEnabled", state.loopEnabled);
        obj.setMember ("loopStartSamples", choc::value::createInt64 (state.loopStart.samples));
        obj.setMember ("loopEndSamples", choc::value::createInt64 (state.loopEnd.samples));
        return obj;
    });

    jsContext_.registerFunction ("__remidy_timeline_set_tempo", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto bpm = args.get<double> (0, 120.0);
        if (bpm > 0.0)
            uapmd::AppModel::instance().timeline().tempo = bpm;
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_timeline_get_clips", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        if (trackIndex < 0)
            return choc::value::createEmptyArray();

        auto tracks = uapmd::AppModel::instance().getTimelineTracks();
        if (trackIndex >= static_cast<int32_t> (tracks.size()))
            return choc::value::createEmptyArray();

        const auto clips = tracks[static_cast<size_t> (trackIndex)]->clipManager().getAllClips();
        auto arr = choc::value::createEmptyArray();
        for (const auto& clip : clips)
        {
            auto obj = choc::value::createObject ("ClipData");
            obj.setMember ("clipId", clip.clipId);
            obj.setMember ("positionSamples", choc::value::createInt64 (clip.position.samples));
            obj.setMember ("durationSamples", choc::value::createInt64 (clip.durationSamples));
            obj.setMember ("name", clip.name);
            obj.setMember ("filepath", clip.filepath);
            obj.setMember ("clipType", clip.clipType == uapmd::ClipType::Midi ? std::string ("midi") : std::string ("audio"));
            obj.setMember ("gain", clip.gain);
            obj.setMember ("muted", clip.muted);
            obj.setMember ("tempo", clip.clipTempo);
            obj.setMember ("tickResolution", static_cast<int32_t> (clip.tickResolution));
            arr.addArrayElement (obj);
        }
        return arr;
    });

    jsContext_.registerFunction ("__remidy_timeline_ensure_dag_graph", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        return choc::value::createBool(uapmd::AppModel::instance().ensureTrackUsesEditorGraph(trackIndex));
    });

    jsContext_.registerFunction ("__remidy_timeline_show_track_graph", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        uapmd::AppModel::instance().requestShowTrackGraph(trackIndex);
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_timeline_revert_track_graph_to_simple", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        return choc::value::createBool(uapmd::AppModel::instance().revertTrackToSimpleGraph(trackIndex));
    });

    jsContext_.registerFunction ("__remidy_timeline_get_track_graph_connections", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        std::vector<uapmd::AudioPluginGraphConnection> connections;
        std::string error;

        auto result = choc::value::createObject ("TrackGraphConnections");
        if (!uapmd::AppModel::instance().getTrackGraphConnections(trackIndex, connections, error)) {
            result.setMember("success", false);
            result.setMember("error", error);
            result.setMember("connections", choc::value::createEmptyArray());
            return result;
        }

        auto array = choc::value::createEmptyArray();
        for (const auto& connection : connections)
            array.addArrayElement(serializeGraphConnection(connection));

        result.setMember("success", true);
        result.setMember("error", "");
        result.setMember("connections", array);
        return result;
    });

    jsContext_.registerFunction ("__remidy_timeline_connect_track_graph", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t>(0, -1);
        auto busTypeText = args.get<std::string>(1, "");
        auto sourceTypeText = args.get<std::string>(2, "");
        auto sourceInstanceId = args.get<int32_t>(3, -1);
        auto sourceBusIndex = args.get<int32_t>(4, 0);
        auto targetTypeText = args.get<std::string>(5, "");
        auto targetInstanceId = args.get<int32_t>(6, -1);
        auto targetBusIndex = args.get<int32_t>(7, 0);

        auto result = choc::value::createObject("TrackGraphMutation");
        uapmd::AudioPluginGraphBusType busType;
        uapmd::AudioPluginGraphEndpointType sourceType, targetType;
        if (!parseGraphBusType(busTypeText, busType) ||
            !parseGraphEndpointType(sourceTypeText, sourceType) ||
            !parseGraphEndpointType(targetTypeText, targetType)) {
            result.setMember("success", false);
            result.setMember("error", "Invalid graph bus or endpoint type");
            return result;
        }

        std::string error;
        const bool ok = uapmd::AppModel::instance().connectTrackGraph(
            trackIndex,
            uapmd::AudioPluginGraphConnection{
                .id = 0,
                .bus_type = busType,
                .source = uapmd::AudioPluginGraphEndpoint{
                    .type = sourceType,
                    .instance_id = sourceInstanceId,
                    .bus_index = static_cast<uint32_t>(std::max(0, sourceBusIndex)),
                },
                .target = uapmd::AudioPluginGraphEndpoint{
                    .type = targetType,
                    .instance_id = targetInstanceId,
                    .bus_index = static_cast<uint32_t>(std::max(0, targetBusIndex)),
                },
            },
            error);
        result.setMember("success", ok);
        result.setMember("error", error);
        return result;
    });

    jsContext_.registerFunction ("__remidy_timeline_disconnect_track_graph_connection", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t>(0, -1);
        auto connectionId = args.get<int64_t>(1, 0);
        std::string error;
        const bool ok = uapmd::AppModel::instance().disconnectTrackGraphConnection(trackIndex, connectionId, error);
        auto result = choc::value::createObject("TrackGraphMutation");
        result.setMember("success", ok);
        result.setMember("error", error);
        return result;
    });

    jsContext_.registerFunction ("__remidy_timeline_get_clip_audio_events", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        auto clipId = args.get<int32_t> (1, -1);
        std::vector<uapmd::ClipMarker> markers;
        std::vector<uapmd::AudioWarpPoint> warps;
        std::string error;
        auto result = choc::value::createObject ("ClipAudioEvents");
        if (!uapmd::AppModel::instance().getClipAudioEvents(trackIndex, clipId, markers, warps, error)) {
            result.setMember ("success", false);
            result.setMember ("error", error);
            return result;
        }

        std::string ownerReferenceId = kMasterMarkerReferenceId.data();
        if (trackIndex != uapmd::kMasterTrackIndex) {
            auto tracks = uapmd::AppModel::instance().getTimelineTracks();
            if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()) && tracks[trackIndex]) {
                if (auto* clip = tracks[trackIndex]->clipManager().getClip(clipId))
                    ownerReferenceId = clip->referenceId;
            }
        }

        auto markerArray = choc::value::createEmptyArray();
        for (const auto& marker : markers)
            markerArray.addArrayElement(serializeMarker(marker, ownerReferenceId));
        auto warpArray = choc::value::createEmptyArray();
        for (const auto& warp : warps)
            warpArray.addArrayElement(serializeWarp(warp, ownerReferenceId));

        result.setMember ("success", true);
        result.setMember ("markers", markerArray);
        result.setMember ("audioWarps", warpArray);
        return result;
    });

    jsContext_.registerFunction ("__remidy_timeline_set_clip_audio_events", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        auto clipId = args.get<int32_t> (1, -1);
        auto result = choc::value::createObject ("ClipAudioEventSetResult");
        if (trackIndex < 0 || clipId < 0 || args.size() < 3 || args[2] == nullptr || !args[2]->isObject()) {
            result.setMember ("success", false);
            result.setMember ("error", "invalid arguments");
            return result;
        }

        auto tracks = uapmd::AppModel::instance().getTimelineTracks();
        std::string ownerReferenceId = kMasterMarkerReferenceId.data();
        if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()) && tracks[trackIndex]) {
            if (auto* clip = tracks[trackIndex]->clipManager().getClip(clipId))
                ownerReferenceId = clip->referenceId;
        }

        std::vector<uapmd::ClipMarker> markers;
        std::vector<uapmd::AudioWarpPoint> warps;
        std::string error;
        auto payload = args[2]->getView();
        auto markersView = payload.hasObjectMember("markers") ? std::optional(payload["markers"]) : std::nullopt;
        auto warpsView = payload.hasObjectMember("audioWarps") ? std::optional(payload["audioWarps"]) : std::nullopt;
        if (!parseMarkersValue(markersView ? &*markersView : nullptr, ownerReferenceId, markers, error) ||
            !parseWarpsValue(warpsView ? &*warpsView : nullptr, ownerReferenceId, warps, error) ||
            !uapmd::AppModel::instance().setClipAudioEvents(trackIndex, clipId, std::move(markers), std::move(warps), error)) {
            result.setMember ("success", false);
            result.setMember ("error", error);
            return result;
        }

        result.setMember ("success", true);
        return result;
    });

    jsContext_.registerFunction ("__remidy_timeline_get_master_markers", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto array = choc::value::createEmptyArray();
        for (const auto& marker : uapmd::AppModel::instance().masterTrackMarkers())
            array.addArrayElement(serializeMarker(marker, kMasterMarkerReferenceId));
        return array;
    });

    jsContext_.registerFunction ("__remidy_timeline_set_master_markers", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto result = choc::value::createObject ("MasterMarkerSetResult");
        std::vector<uapmd::ClipMarker> markers;
        std::string error;
        auto markersView = (args.size() > 0 && args[0] != nullptr) ? std::optional(args[0]->getView()) : std::nullopt;
        if (!parseMarkersValue(markersView ? &*markersView : nullptr, kMasterMarkerReferenceId, markers, error) ||
            !uapmd::AppModel::instance().setMasterTrackMarkersWithValidation(std::move(markers), error)) {
            result.setMember ("success", false);
            result.setMember ("error", error);
            return result;
        }
        result.setMember ("success", true);
        return result;
    });

    jsContext_.registerFunction ("__remidy_timeline_add_midi_clip", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        auto positionSamples = args.get<int64_t> (1, 0);
        auto filepath = args.get<std::string> (2, "");

        auto makeError = [] (const std::string& msg) {
            auto obj = choc::value::createObject ("ClipAddResult");
            obj.setMember ("clipId", -1);
            obj.setMember ("success", false);
            obj.setMember ("error", msg);
            return obj;
        };

        if (trackIndex < 0 || filepath.empty())
            return makeError ("invalid arguments");

        auto& appModel = uapmd::AppModel::instance();
        uapmd::TimelinePosition pos{};
        pos.samples = positionSamples;
        auto result = appModel.addMidiClipToTrack (trackIndex, pos, filepath);

        auto obj = choc::value::createObject ("ClipAddResult");
        obj.setMember ("clipId", result.clipId);
        obj.setMember ("success", result.success);
        obj.setMember ("error", result.error);
        return obj;
    });

    jsContext_.registerFunction ("__remidy_timeline_remove_clip", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        auto clipId = args.get<int32_t> (1, -1);
        if (trackIndex < 0 || clipId < 0)
            return choc::value::createBool (false);
        return choc::value::createBool (uapmd::AppModel::instance().removeClipFromTrack (trackIndex, clipId));
    });

    jsContext_.registerFunction ("__remidy_timeline_get_clip_ump_events", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        auto clipId     = args.get<int32_t> (1, -1);
        if (trackIndex < 0 || clipId < 0)
            return choc::value::createObject ("");
        return uapmd::AppModel::instance().getMidiClipUmpEvents (trackIndex, clipId);
    });

    jsContext_.registerFunction ("__remidy_timeline_add_ump_event", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex = args.get<int32_t> (0, -1);
        auto clipId     = args.get<int32_t> (1, -1);
        auto tick       = static_cast<uint64_t>(args.get<int64_t> (2, 0));
        if (trackIndex < 0 || clipId < 0)
            return choc::value::createBool (false);
        // args[3] is an array of 1–2 UMP words
        std::vector<uint32_t> words;
        if (args.size() > 3) {
            auto* wordsVal = args[3];
            if (wordsVal && wordsVal->isArray())
                for (uint32_t i = 0; i < wordsVal->size(); ++i)
                    words.push_back (static_cast<uint32_t>((*wordsVal)[i].get<int64_t>()));
        }
        if (words.empty()) return choc::value::createBool (false);
        std::string error;
        return choc::value::createBool (
            uapmd::AppModel::instance().addUmpEventToClip (trackIndex, clipId, tick, std::move(words), error));
    });

    jsContext_.registerFunction ("__remidy_timeline_remove_ump_event", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex  = args.get<int32_t> (0, -1);
        auto clipId      = args.get<int32_t> (1, -1);
        auto eventIndex  = args.get<int32_t> (2, -1);
        if (trackIndex < 0 || clipId < 0 || eventIndex < 0)
            return choc::value::createBool (false);
        std::string error;
        return choc::value::createBool (
            uapmd::AppModel::instance().removeUmpEventFromClip (trackIndex, clipId, eventIndex, error));
    });

    jsContext_.registerFunction ("__remidy_timeline_create_empty_midi_clip", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto trackIndex      = args.get<int32_t> (0, -1);
        if (trackIndex < 0) return choc::value::createInt32 (-1);
        auto positionSamples = args.get<int64_t>  (1, 0);
        auto tickResolution  = static_cast<uint32_t>(std::max (1, args.get<int32_t> (2, 480)));
        auto bpm             = args.get<double>   (3, 120.0);
        auto r = uapmd::AppModel::instance().createEmptyMidiClip (
            trackIndex, positionSamples, tickResolution, bpm);
        return choc::value::createInt32 (r.clipId);
    });
}

void UapmdJSRuntime::registerRenderAPI()
{
    jsContext_.registerFunction ("__remidy_render_start", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto outputPath = args.get<std::string> (0, "");
        if (outputPath.empty())
            return choc::value::createBool (false);

        uapmd::AppModel::RenderToFileSettings settings;
        settings.outputPath = outputPath;
        settings.startSeconds = std::max (0.0, args.get<double> (1, 0.0));

        auto endSeconds = args.get<double> (2, -1.0);
        if (endSeconds > settings.startSeconds)
            settings.endSeconds = endSeconds;

        settings.tailSeconds = std::max (0.0, args.get<double> (3, 2.0));
        settings.useContentFallback = args.get<bool> (4, true);

        auto bounds = uapmd::AppModel::instance().timelineContentBounds();
        settings.contentBoundsValid = bounds.hasContent;
        settings.contentStartSeconds = bounds.startSeconds;
        settings.contentEndSeconds = bounds.endSeconds;

        return choc::value::createBool (uapmd::AppModel::instance().startRenderToFile (settings));
    });

    jsContext_.registerFunction ("__remidy_render_get_status", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto status = uapmd::AppModel::instance().getRenderToFileStatus();
        auto obj = choc::value::createObject ("RenderToFileStatus");
        obj.setMember ("running", status.running);
        obj.setMember ("completed", status.completed);
        obj.setMember ("success", status.success);
        obj.setMember ("progress", status.progress);
        obj.setMember ("renderedSeconds", status.renderedSeconds);
        obj.setMember ("message", status.message);
        obj.setMember ("outputPath", status.outputPath.string());
        return obj;
    });

    jsContext_.registerFunction ("__remidy_render_clear_status", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        uapmd::AppModel::instance().clearCompletedRenderStatus();
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_render_cancel", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        uapmd::AppModel::instance().cancelRenderToFile();
        return choc::value::Value();
    });
}

} // namespace uapmd
