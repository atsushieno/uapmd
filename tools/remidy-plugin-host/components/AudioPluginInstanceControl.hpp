#pragma once

#include "remidy/remidy.hpp"
#include "../WebViewProxy.hpp"

namespace uapmd {

    // invoked via JS callbacks too.
    void instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId);
    void registerPluginInstanceControlFeatures(WebViewProxy& proxy);
}
