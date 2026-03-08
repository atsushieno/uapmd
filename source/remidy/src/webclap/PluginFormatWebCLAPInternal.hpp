#pragma once

#include "remidy/priv/plugin-format-webclap.hpp"

#if defined(__EMSCRIPTEN__)

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace remidy::webclap {

    using InstantiateCallback = std::function<void(std::unique_ptr<PluginInstance>, std::string)>;

    int32_t enqueuePendingInstantiation(PluginCatalogEntry* entry,
                                        PluginFormat::PluginInstantiationOptions options,
                                        InstantiateCallback callback);

    void failPendingInstantiation(int32_t token, const std::string& errorMessage);

    bool requestInstanceFromJs(int32_t token, const std::string& bundleUrl, const std::string& pluginId);

    std::string resolveManifestPayload();

} // namespace remidy::webclap

#endif
