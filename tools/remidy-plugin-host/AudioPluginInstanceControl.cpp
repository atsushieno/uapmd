
#include "AppModel.hpp"
#include "AudioPluginInstanceControl.hpp"
#include <choc/text/choc_JSON.h>

void uapmd::instantiatePlugin(const std::string_view& format, const std::string_view& pluginId) {
    auto scanner = AppModel::instance().pluginScanning;
    AppModel::instance().pluginInstancing = std::make_unique<remidy_tooling::PluginInstancing>(*scanner, format, pluginId);
    AppModel::instance().pluginInstancing->makeAlive([format,pluginId](std::string){
        std::cerr << "Instantiated plugin " << format << " : " << pluginId << std::endl;
    });
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
