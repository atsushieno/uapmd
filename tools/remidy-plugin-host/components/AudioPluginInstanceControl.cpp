
#include "../AppModel.hpp"
#include "AudioPluginInstanceControl.hpp"
#include "choc/text/choc_JSON.h"

void uapmd::instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId) {
    AppModel::instance().instantiatePlugin(instancingId, format, pluginId);
}

std::vector<uapmd::ParameterMetadata> uapmd::getParameterList(int32_t instanceId) {
    return AppModel::instance().getParameterList(instanceId);
}

void uapmd::registerPluginInstanceControlFeatures(remidy::webui::WebViewProxy& proxy) {
    proxy.registerFunction("remidy_instantiatePlugin", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto instancingId = req["instancingId"].getInt64();
        auto format = req["format"].getString();
        auto pluginId = req["pluginId"].getString();
        instantiatePlugin(instancingId, format, pluginId);
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
                                             "id", p.id,
                                             "name", p.name,
                                             "path", p.path,
                                             "initialValue", p.initialValue,
                                             "hidden", p.hidden);
        });
        return choc::json::toString(arr);
    });

    proxy.registerFunction("remidy_sendNoteOn", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        AppModel::instance().sendNoteOn(req["instanceId"].getInt64(), req["note"].getInt64());
        return "";
    });

    proxy.registerFunction("remidy_sendNoteOff", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        AppModel::instance().sendNoteOff(req["instanceId"].getInt64(), req["note"].getInt64());
        return "";
    });
}
