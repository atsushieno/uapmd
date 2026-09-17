#pragma once

// Conversions between the uapmd_plugin_hosting plugin format abstraction and the remidy
// types it is implemented over. This header is private to the module: the public
// AudioPluginFormat / AudioPluginScanning / AudioPluginCatalog headers stay remidy-free.

#include "remidy/remidy.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd_plugin_hosting {

    AudioPluginCatalogEntry toAudioPluginCatalogEntry(const remidy::PluginCatalogEntry& source);
    remidy::PluginCatalogEntry toRemidyCatalogEntry(const AudioPluginCatalogEntry& source);

    AudioPluginUIThreadRequirement toAudioPluginUIThreadRequirement(remidy::PluginUIThreadRequirement source);
    remidy::PluginUIThreadRequirement toRemidyUIThreadRequirement(AudioPluginUIThreadRequirement source);

}
