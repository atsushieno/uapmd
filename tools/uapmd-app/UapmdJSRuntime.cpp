#include "UapmdJSRuntime.hpp"
#include "AppModel.hpp"
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
}

void UapmdJSRuntime::reinitialize()
{
    jsContext_ = choc::javascript::createQuickJSContext();

    registerConsoleFunctions();
    registerPluginCatalogAPI();
    registerPluginScanToolAPI();
    registerPluginInstanceAPI();
    registerSequencerMidiAPI();
    registerSequencerTransportAPI();
    registerSequencerInstanceAPI();
    registerSequencerAudioAnalysisAPI();
    registerSequencerAudioDeviceAPI();
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

void UapmdJSRuntime::registerPluginCatalogAPI()
{
    jsContext_.registerFunction ("__remidy_catalog_get_count", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto& catalog = sequencer.engine()->catalog();
        auto plugins = catalog.getPlugins();
        return choc::value::createInt32 (static_cast<int32_t>(plugins.size()));
    });

    jsContext_.registerFunction ("__remidy_catalog_get_plugin_at", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto index = args.get<int32_t> (0, -1);
        if (index < 0)
            return choc::value::Value();

        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto& catalog = sequencer.engine()->catalog();
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
        auto& catalog = sequencer.engine()->catalog();
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
        auto& catalog = sequencer.engine()->catalog();
        std::filesystem::path path (pathStr);
        catalog.save (path);
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

        auto result = uapmd::AppModel::instance().savePluginState (instanceId, filepath);

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

        auto result = uapmd::AppModel::instance().loadPluginState (instanceId, filepath);

        auto obj = choc::value::createObject ("PluginStateResult");
        obj.setMember ("success", result.success);
        obj.setMember ("error", result.error);
        obj.setMember ("filepath", result.filepath);
        obj.setMember ("instanceId", result.instanceId);
        return obj;
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
        auto position = sequencer.playbackPositionSamples();
        return choc::value::createInt64 (position);
    });
}

void UapmdJSRuntime::registerSequencerInstanceAPI()
{
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
        auto name = sequencer.engine()->getPluginName (instanceId);
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
        auto bypassed = sequencer.engine()->isPluginBypassed (instanceId);
        return choc::value::createBool (bypassed);
    });

    jsContext_.registerFunction ("__remidy_sequencer_setPluginBypassed", [] (choc::javascript::ArgumentList args) -> choc::value::Value
    {
        auto instanceId = args.get<int32_t> (0, -1);
        auto bypassed = args.get<bool> (1, false);

        if (instanceId >= 0)
        {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            sequencer.engine()->setPluginBypassed (instanceId, bypassed);
        }
        return choc::value::Value();
    });

    jsContext_.registerFunction ("__remidy_sequencer_getTrackInfos", [] (choc::javascript::ArgumentList) -> choc::value::Value
    {
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto trackInfos = sequencer.engine()->getTrackInfos();

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
        auto updates = sequencer.engine()->getParameterUpdates (instanceId);

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

} // namespace uapmd
