#pragma once

#include "remidy/remidy.hpp"
#include "../WebViewProxy.hpp"

namespace uapmd {
    class AudioPluginViewEntry {
    public:
        std::string format{};
        std::string id{};
        std::string name{};
        std::string vendor{};
    };

    class AudioPluginViewEntryList {
        remidy::PluginCatalog& catalog;
    public:
        explicit AudioPluginViewEntryList(remidy::PluginCatalog& catalog) : catalog(catalog) {}

        std::vector<AudioPluginViewEntry> entries() {
            return catalog.getPlugins() | std::views::transform([](remidy::PluginCatalogEntry *e) {
                return AudioPluginViewEntry{
                        .format = e->format(),
                        .id = e->pluginId(),
                        .name = e->displayName(),
                        .vendor = e->vendorName()
                };
            }) | std::ranges::to<std::vector>();
        }
        std::vector<AudioPluginViewEntry> denyList() {
            return catalog.getDenyList() | std::views::transform([](remidy::PluginCatalogEntry *e) {
                return AudioPluginViewEntry{
                        .format = e->format(),
                        .id = e->pluginId(),
                        .name = e->displayName(),
                        .vendor = e->vendorName()
                };
            }) | std::ranges::to<std::vector>();
        }
    };

    // invoked via JS callbacks too.
    AudioPluginViewEntryList getPluginViewEntryList();
    void registerPluginViewEntryListFeatures(WebViewProxy& proxy);
}
