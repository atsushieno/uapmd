#pragma once

#include "remidy/remidy.hpp"
#include "WebViewProxy.hpp"

namespace uapmd {

    // invoked via JS callbacks too.
    void instantiatePlugin(const std::string_view& format, const std::string_view& pluginId);
    void registerPluginInstanceControlFeatures(WebViewProxy& proxy);
}
