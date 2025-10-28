
#include "AudioPluginInstanceControl.hpp"
#include "choc/text/choc_JSON.h"
#include "../AppModel.hpp"
#include <iostream>

void uapmd::addSimplePluginTrack(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId) {
    AppModel::instance().addSimplePluginTrack(instancingId, format, pluginId);
}

std::vector<uapmd::ParameterMetadata> uapmd::getParameterList(int32_t instanceId) {
    return AppModel::instance().sequencer().getParameterList(instanceId);
}

std::vector<uapmd::PresetsMetadata> uapmd::getPresetList(int32_t instanceId) {
    return AppModel::instance().sequencer().getPresetList(instanceId);
}

void uapmd::loadPreset(int32_t instanceId, int32_t presetIndex) {
    AppModel::instance().sequencer().loadPreset(instanceId, presetIndex);
}

void uapmd::registerPluginInstanceControlFeatures(remidy::webui::WebViewProxy& proxy) {
    proxy.registerFunction("remidy_instantiatePlugin", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto instancingId = req["instancingId"].getInt64();
        auto format = req["format"].getString();
        auto pluginId = req["pluginId"].getString();
        addSimplePluginTrack(instancingId, format, pluginId);
        return "";
    });
    AppModel::instance().instancingCompleted.emplace_back([&proxy](int32_t instancingId, int32_t instanceId, std::string error) {
        proxy.evalJS(std::format("var e = new Event('RemidyInstancingCompleted'); e.instancingId = {}; e.instanceId = {}; window.dispatchEvent(e)", instancingId, instanceId));
    });

    proxy.registerFunction("remidy_getPluginParameterList", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto instanceId = req["instanceId"].getInt64();
        auto pl = getParameterList(instanceId);
        auto arr = choc::value::createArray(pl.size(), [pl](int32_t index) {
            auto p = pl[index];
            return choc::value::createObject("ParameterMetadata",
                "index", (double) p.index,
                "stableId", p.stableId,
                "name", p.name,
                "path", p.path,
                "initialValue", p.initialValue,
                "hidden", p.hidden);
        });
        return choc::json::toString(arr);
    });

    proxy.registerFunction("remidy_getPluginPresetList", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto instanceId = req["instanceId"].getInt64();
        auto pl = getPresetList(instanceId);
        auto arr = choc::value::createArray(pl.size(), [pl](int32_t index) {
            auto p = pl[index];
            return choc::value::createObject("PresetsMetadata",
                "bank", (double) p.bank,
                "index", (double) p.index,
                "stableId", p.stableId,
                "name", p.name,
                "path", p.path);
        });
        return choc::json::toString(arr);
    });

    proxy.registerFunction("remidy_sendNoteOn", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        AppModel::instance().sequencer().sendNoteOn(req["trackIndex"].getInt64(), req["note"].getInt64());
        return "";
    });

    proxy.registerFunction("remidy_sendNoteOff", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        AppModel::instance().sequencer().sendNoteOff(req["trackIndex"].getInt64(), req["note"].getInt64());
        return "";
    });

    proxy.registerFunction("remidy_setParameterValue", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto instanceId = req["instanceId"].getInt64();
        auto index = req["index"].getInt64();
        // Either the UI treats 0.0 as int64, or choc::value treats 0.0 as int64, or my saucer interceptor does,
        // and causes "Value is not float64" error...
        auto value = req["value"].getType().isInt64() ? req["value"].getInt64() : req["value"].getFloat64();
        AppModel::instance().sequencer().setParameterValue(instanceId, index, value);
        return "";
    });

    proxy.registerFunction("remidy_savePluginState", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto ret = AppModel::instance().sequencer().saveState();
        std::cerr << "Saved state. size: " << ret.size() << std::endl;
        return std::string{reinterpret_cast<char *>(ret.data())};
    });

    proxy.registerFunction("remidy_loadPluginState", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto state = req["state"].getString();
        std::vector<uint8_t> stateBytes{(uint8_t*) state.begin(), (uint8_t*) state.begin() + state.length()};
        AppModel::instance().sequencer().loadState(stateBytes);
        std::cerr << "Loaded state." << std::endl;
        return "";
    });

    proxy.registerFunction("remidy_loadPreset", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto instanceId = req["instanceId"].getInt64();
        auto presetIndex = req["presetIndex"].getInt64();
        loadPreset(instanceId, presetIndex);
        return "";
    });
}
