
#include "AppModel.hpp"
#include "AudioPluginInstanceControl.hpp"
#include <choc/text/choc_JSON.h>

void uapmd::instantiatePlugin(const std::string_view& format, const std::string_view& pluginId) {
    AppModel::instance().instantiatePlugin(format, pluginId);
}

void uapmd::registerPluginInstanceControlFeatures(WebViewProxy& proxy) {
    proxy.registerFunction("remidy_instantiatePlugin", [](const std::string_view& args) -> std::string {
        auto req = choc::json::parse(args);
        auto format = req["format"].getString();
        auto pluginId = req["pluginId"].getString();
        instantiatePlugin(format, pluginId);
        return "";
    });
}
