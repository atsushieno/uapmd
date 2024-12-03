
#include "AppModel.hpp"
#include "AudioPluginInstanceControl.hpp"
#include <choc/text/choc_JSON.h>

void uapmd::instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId) {
    AppModel::instance().instantiatePlugin(instancingId, format, pluginId);
}

void uapmd::registerPluginInstanceControlFeatures(WebViewProxy& proxy) {
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
}
