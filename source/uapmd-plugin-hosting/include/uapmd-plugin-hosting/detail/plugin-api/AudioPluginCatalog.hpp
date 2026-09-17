#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace uapmd_plugin_hosting {

    // Describes one plugin that some AudioPluginFormat can instantiate.
    //
    // It is the uapmd_plugin_hosting counterpart of remidy::PluginCatalogEntry, and it is a
    // plain value type: everything here is what a scanner discovered about the plugin, and
    // the format is expected to be able to instantiate the plugin from `format` + `pluginId`
    // alone (plus `bundlePath` for bundle-based formats).
    class AudioPluginCatalogEntry {
        std::string format_{};
        std::string plugin_id_{};
        std::string display_name_{};
        std::string vendor_name_{};
        std::string product_url_{};
        std::filesystem::path bundle_path_{};

    public:
        // The format name, matching AudioPluginFormat::name().
        const std::string& format() const { return format_; }
        void format(const std::string& value) { format_ = value; }
        // The format-defined plugin identifier. It must be stable across scans.
        const std::string& pluginId() const { return plugin_id_; }
        void pluginId(const std::string& value) { plugin_id_ = value; }
        const std::string& displayName() const { return display_name_; }
        void displayName(const std::string& value) { display_name_ = value; }
        const std::string& vendorName() const { return vendor_name_; }
        void vendorName(const std::string& value) { vendor_name_ = value; }
        const std::string& productUrl() const { return product_url_; }
        void productUrl(const std::string& value) { product_url_ = value; }
        // A file system path, or a URL for formats whose plugins are remote (WebCLAP).
        // It is empty for formats that have no bundle concept (AU).
        const std::filesystem::path& bundlePath() const { return bundle_path_; }
        void bundlePath(const std::filesystem::path& value);
    };

    // Owns the plugin entries that scanning discovered.
    // It is the uapmd_plugin_hosting counterpart of remidy::PluginCatalog. The plugin list
    // cache file it reads and writes is the same format the remidy scanner uses.
    class AudioPluginCatalog {
        std::vector<AudioPluginCatalogEntry> entries_{};
        std::vector<AudioPluginCatalogEntry> deny_list_{};

    public:
        std::vector<AudioPluginCatalogEntry*> getPlugins();
        std::vector<AudioPluginCatalogEntry*> getDenyList();
        bool contains(const std::string& format, const std::string& pluginId) const;
        void add(AudioPluginCatalogEntry entry);
        void merge(AudioPluginCatalog&& other);
        void clear();
        void load(const std::filesystem::path& path);
        void save(const std::filesystem::path& path);
    };

}
