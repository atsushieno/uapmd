#include "PluginFormatAAP.hpp"
#include "priv/plugin-format-aap.hpp"

#include <aap/core/host/plugin-client-system.h>
#include <aap/core/host/android/audio-plugin-host-android.h>

std::unique_ptr<remidy::PluginFormatAAP>
remidy::PluginFormatAAP::create() {
    return std::make_unique<PluginFormatAAPImpl>();
}

remidy::PluginFormatAAPImpl::PluginFormatAAPImpl() {
    plugin_list_snapshot = aap::PluginListSnapshot::queryServices();
    // FIXME: retrieve serviceConnectorInstanceId, not 0
    plugin_client_connections = aap::getPluginConnectionListByConnectorInstanceId(0, true);
    android_host = std::make_unique<aap::PluginClient>(plugin_client_connections, &plugin_list_snapshot);
}

const aap::PluginInformation *
findPluginInformationFrom(remidy::PluginCatalogEntry *info) {
    auto list = aap::PluginListSnapshot::queryServices();
    return list.getPluginInformation(info->pluginId());
}

void remidy::PluginFormatAAPImpl::createInstance(remidy::PluginCatalogEntry *info,
                                             remidy::PluginFormat::PluginInstantiationOptions options,
                                             std::function<void(std::unique_ptr<PluginInstance>,
                                                                std::string)> callback) {
    const aap::PluginInformation* pluginInfo;
    remidy::EventLoop::runTaskOnMainThread([&] {
        pluginInfo = findPluginInformationFrom(info);
    });
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
        int sampleRate = (int) 44100;
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
            // Not sure if this is good, in aap-juce it must be NON-main thread (as JUCE createPluginInstance() is invoked on the main thread)
            remidy::EventLoop::runTaskOnMainThread([this, pluginInfo, cb] {
                aap::PluginClientSystem::getInstance()->ensurePluginServiceConnected(plugin_client_connections, pluginInfo->getPluginPackageName(), cb);
            });
        }
    }
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
