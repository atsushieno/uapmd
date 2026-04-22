#include "PluginFormatAAP.hpp"
#include "detail/plugin-format-aap.hpp"

#include <aap/core/host/plugin-client-system.h>
#include <aap/core/host/android/audio-plugin-host-android.h>

namespace remidy {

std::unique_ptr<PluginFormatAAP>
PluginFormatAAP::create() {
    return std::make_unique<PluginFormatAAPImpl>();
}

PluginFormatAAPImpl::PluginFormatAAPImpl() {
    plugin_list_snapshot = aap::PluginListSnapshot::queryServices();
    // FIXME: retrieve serviceConnectorInstanceId, not 0
    plugin_client_connections = aap::getPluginConnectionListByConnectorInstanceId(0, true);
    android_host = std::make_unique<aap::PluginClient>(plugin_client_connections, &plugin_list_snapshot);
}

void PluginFormatAAPImpl::destroyInstance(aap::PluginInstance* instance) {
    if (!android_host || !instance)
        return;
    android_host->destroyInstance(instance);
}

const aap::PluginInformation *
findPluginInformationFrom(PluginCatalogEntry *info) {
    auto list = aap::PluginListSnapshot::queryServices();
    return list.getPluginInformation(info->pluginId());
}

void PluginFormatAAPImpl::createInstance(PluginCatalogEntry *info,
                                             PluginFormat::PluginInstantiationOptions options,
                                             std::function<void(std::unique_ptr<PluginInstance>,
                                                                std::string)> callback) {
    (void) options;
    const aap::PluginInformation* pluginInfo = findPluginInformationFrom(info);

    if (pluginInfo == nullptr) {
        std::string error = std::format("Android Audio Plugin {} was not found.", info->displayName());
        callback(nullptr, error);
    } else {
        // If the plugin service is not connected yet, then connect asynchronously with the callback
        // that processes instancing and invoke user callback (PluginCreationCallback).
        std::function<void(int32_t,std::string&)> aapCallback = [this, info, callback](int32_t instanceID, std::string& error) {
            auto androidInstance = android_host->getInstanceById(instanceID);
            callback(std::make_unique<PluginInstanceAAP>(this, info, androidInstance), error);
        };
        // FIXME: just like LV2, we have to pass initial sample rate at instantiation time,
        //  which does not match what remidy passes at `createInstance()` (it is passed at `configure()`).
        //  Until then, we have to pass a stub value.
        int sampleRate = (int) 48000;
        auto identifier = pluginInfo->getPluginID();
        auto service = plugin_client_connections->getServiceHandleForConnectedPlugin(pluginInfo->getPluginPackageName(), pluginInfo->getPluginLocalName());
        if (service != nullptr) {
            auto result = android_host->createInstance(identifier, sampleRate, true);
            aapCallback(result.value, result.error);
        } else {
            std::function<void(std::string&)> cb = [identifier,sampleRate,aapCallback,this](std::string& error) {
                if (error.empty()) {
                    auto result = android_host->createInstance(identifier, sampleRate, true);
                    aapCallback(result.value, result.error);
                }
                else
                    aapCallback(-1, error);
            };
            aap::PluginClientSystem::getInstance()->ensurePluginServiceConnected(plugin_client_connections, pluginInfo->getPluginPackageName(), cb);
        }
    }
}

std::vector<PluginCatalogEntry>
PluginScanningAAP::getAllFastScannablePlugins() {
    std::vector<PluginCatalogEntry> ret{};
    auto infos = aap::PluginClientSystem::getInstance()->getInstalledPlugins();
    for (auto* plugin : infos) {
        PluginCatalogEntry e{};
        std::string format = owner->name();
        e.format(format);
        std::string id = plugin->getPluginID();
        e.pluginId(id);
        e.displayName(plugin->getDisplayName());
        e.vendorName(plugin->getDeveloperName());
        ret.push_back(std::move(e));
    }
    return ret;
}

void PluginScanningAAP::startSlowPluginScan(std::function<void(PluginCatalogEntry entry)> /*pluginFound*/,
                                                    PluginScanCompletedCallback scanCompleted) {
    if (scanCompleted)
        scanCompleted("");
}

} // namespace remidy
