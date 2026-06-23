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
        auto resolveCreatedInstance = [this, info](size_t previousInstanceCount, int32_t instanceID) -> aap::PluginInstance* {
            auto matches = [info, instanceID](aap::PluginInstance* instance) {
                return instance &&
                       instance->getInstanceId() == instanceID &&
                       instance->getPluginInformation() &&
                       instance->getPluginInformation()->getPluginID() == info->pluginId();
            };

            if (android_host->getInstanceCount() > previousInstanceCount) {
                auto* created = android_host->getInstanceByIndex(static_cast<int32_t>(previousInstanceCount));
                if (matches(created))
                    return created;
            }

            auto* byId = android_host->getInstanceById(instanceID);
            return matches(byId) ? byId : nullptr;
        };

        std::function<void(size_t,int32_t,std::string&)> aapCallback = [this, info, callback, resolveCreatedInstance](size_t previousInstanceCount, int32_t instanceID, std::string& error) {
            if (error.empty()) {
                auto androidInstance = resolveCreatedInstance(previousInstanceCount, instanceID);
                if (!androidInstance) {
                    callback(nullptr, std::format("Created AAP instance {} did not match plugin {}.", instanceID, info->pluginId()));
                    return;
                }
                callback(std::make_unique<PluginInstanceAAP>(this, info, androidInstance), "");
            } else
                callback(nullptr, error);
        };
        // FIXME: just like LV2, we have to pass initial sample rate at instantiation time,
        //  which does not match what remidy passes at `createInstance()` (it is passed at `configure()`).
        //  Until then, we have to pass a stub value.
        int sampleRate = (int) 48000;
        auto identifier = pluginInfo->getPluginID();
        auto service = plugin_client_connections->getServiceHandleForConnectedPlugin(pluginInfo->getPluginPackageName(), pluginInfo->getPluginLocalName());
        if (service != nullptr) {
            auto previousInstanceCount = android_host->getInstanceCount();
            auto result = android_host->createInstance(identifier, true);
            aapCallback(previousInstanceCount, result.value, result.error);
        } else {
            std::function<void(std::string&)> cb = [identifier,aapCallback,this](std::string& error) {
                if (error.empty()) {
                    auto previousInstanceCount = android_host->getInstanceCount();
                    auto result = android_host->createInstance(identifier, true);
                    aapCallback(previousInstanceCount, result.value, result.error);
                }
                else
                    aapCallback(android_host->getInstanceCount(), -1, error);
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
