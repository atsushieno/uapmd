#include "PluginFormatAAP.hpp"
#include "priv/plugin-format-aap.hpp"

#include <aap/core/host/plugin-client-system.h>

std::unique_ptr<remidy::PluginFormatAAP>
remidy::PluginFormatAAP::create() {
    return std::make_unique<PluginFormatAAPImpl>();
}

remidy::PluginFormatAAPImpl::PluginFormatAAPImpl() {
}

void remidy::PluginFormatAAPImpl::createInstance(remidy::PluginCatalogEntry *info,
                                             remidy::PluginFormat::PluginInstantiationOptions options,
                                             std::function<void(std::unique_ptr<PluginInstance>,
                                                                std::string)> callback) {

}

remidy::PluginScanningAAP::PluginScanningAAP(remidy::PluginFormatAAPImpl *owner) : owner(owner) {
}

remidy::PluginScanning::ScanningStrategyValue remidy::PluginScanningAAP::scanRequiresLoadLibrary() {
    return ScanningStrategyValue::NEVER;
}

remidy::PluginScanning::ScanningStrategyValue
remidy::PluginScanningAAP::scanRequiresInstantiation() {
    return ScanningStrategyValue::NEVER;
}

std::vector<std::unique_ptr<remidy::PluginCatalogEntry>>
remidy::PluginScanningAAP::scanAllAvailablePlugins(bool requireFastScanning) {
    std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};
    auto infos = aap::PluginClientSystem::getInstance()->getInstalledPlugins();
    for (auto& plugin : infos) {
        auto e = std::make_unique<remidy::PluginCatalogEntry>();
        std::string format = owner->name();
        e->format(format);
        std::string id = plugin->getPluginID();
        e->pluginId(id);
        e->displayName(plugin->getDisplayName());
        e->vendorName(plugin->getDeveloperName());
        ret.push_back(std::move(e));
    }
    return ret;
}
