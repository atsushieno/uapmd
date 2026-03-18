#include "PluginFormatLV2.hpp"

namespace remidy {
    PluginFormatLV2Impl::PluginFormatLV2Impl(std::vector<std::string>& overrideSearchPaths) :
        logger(Logger::global()),
        extensibility(*this) {
        world = lilv_world_new();
        scanning_ = PluginScannerLV2(world);
        // FIXME: setup paths
        lilv_world_load_all(world);

        // This also initializes features
        worldContext = new remidy_lv2::LV2ImplWorldContext(logger, world);
    }
    PluginFormatLV2Impl::~PluginFormatLV2Impl() {
        delete worldContext;
        lilv_free(world);
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> PluginScannerLV2::scanAllAvailablePlugins(bool /*requireFastScanning*/) {
        std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};

        auto plugins = lilv_world_get_all_plugins(world);
        LILV_FOREACH(plugins, iter, plugins) {
            const LilvPlugin* plugin = lilv_plugins_get(plugins, iter);
            auto entry = std::make_unique<PluginCatalogEntry>();
            static std::string lv2Format{"LV2"};
            entry->format(lv2Format);
            auto uriNode = lilv_plugin_get_uri(plugin);
            std::string uri = lilv_node_as_uri(uriNode);
            auto bundleUriNode = lilv_plugin_get_bundle_uri(plugin);
            auto bundlePath = lilv_node_as_uri(bundleUriNode);
            entry->bundlePath(std::filesystem::path{bundlePath});
            entry->pluginId(uri);
            auto nameNode = lilv_plugin_get_name(plugin);
            std::string name = lilv_node_as_string(nameNode);
            auto authorNameNode = lilv_plugin_get_author_name(plugin);
            auto authorName = lilv_node_as_string(authorNameNode);
            auto authorUrlNode = lilv_plugin_get_author_homepage(plugin);
            auto authorUrl = lilv_node_as_string(authorUrlNode);
            entry->displayName(name);
            if (authorName)
                entry->vendorName(authorName);
            if (authorUrl)
                entry->productUrl(authorUrl);
            ret.emplace_back(std::move(entry));
        }
        return ret;
    }

    void PluginFormatLV2Impl::createInstance(
        PluginCatalogEntry* info,
        PluginInstantiationOptions options,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
    ) {
        auto targetUri = lilv_new_uri(world, info->pluginId().c_str());
        auto plugins = lilv_world_get_all_plugins(world);
        auto plugin = lilv_plugins_get_by_uri(plugins, targetUri);
        if (plugin) {
            auto instance = std::make_unique<PluginInstanceLV2>(info, this, plugin);
            callback(std::move(instance), "");
            return;
        }
        callback(nullptr, std::string{"Plugin '"} + info->pluginId() + "' was not found");
    }

    void PluginFormatLV2Impl::unrefLibrary(PluginCatalogEntry& info) {
    }

    PluginCatalog PluginFormatLV2Impl::createCatalogFragment(const std::filesystem::path &bundlePath) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::createCatalogFragment() is not implemented");
    }

    PluginExtensibility<PluginFormat> * PluginFormatLV2Impl::getExtensibility() {
        return &extensibility;
    }

    std::unique_ptr<PluginFormatLV2> PluginFormatLV2::create(std::vector<std::string>& overrideSearchPaths) {
        return std::make_unique<PluginFormatLV2Impl>(overrideSearchPaths);
    }


    PluginFormatLV2::Extensibility::Extensibility(PluginFormat &format) :
        PluginExtensibility(format) {
    }
}
