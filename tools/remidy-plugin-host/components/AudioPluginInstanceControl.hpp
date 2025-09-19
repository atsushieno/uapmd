#pragma once

#include "remidy-webui/WebViewProxy.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {

    // invoked via JS callbacks too.
    void instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId);
    std::vector<uapmd::ParameterMetadata> getParameterList(int32_t instanceId);
    std::vector<uapmd::PresetsMetadata> getPresetList(int32_t instanceId);
    void registerPluginInstanceControlFeatures(remidy::webui::WebViewProxy& proxy);
}
