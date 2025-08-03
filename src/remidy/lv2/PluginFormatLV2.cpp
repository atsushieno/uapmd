#include "PluginFormatLV2.hpp"
#include "cmidi2.h"

namespace remidy {
    PluginFormatLV2::Impl::Impl(PluginFormatLV2* owner) :
        owner(owner),
        logger(Logger::global()),
        extensibility(*owner) {
        world = lilv_world_new();
        scanning_ = AudioPluginScannerLV2(world);
        // FIXME: setup paths
        lilv_world_load_all(world);

        // This also initializes features
        worldContext = new remidy_lv2::LV2ImplWorldContext(logger, world);
    }
    PluginFormatLV2::Impl::~Impl() {
        delete worldContext;
        lilv_free(world);
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> AudioPluginScannerLV2::scanAllAvailablePlugins() {
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
            entry->vendorName(authorName);
            entry->productUrl(authorUrl);
            ret.emplace_back(std::move(entry));
        }
        return ret;
    }

    void PluginFormatLV2::Impl::createInstance(
        PluginCatalogEntry* info,
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

    void PluginFormatLV2::Impl::unrefLibrary(PluginCatalogEntry& info) {
    }

    PluginCatalog PluginFormatLV2::Impl::createCatalogFragment(const std::filesystem::path &bundlePath) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::createCatalogFragment() is not implemented");
    }

    PluginExtensibility<PluginFormat> * PluginFormatLV2::Impl::getExtensibility() {
        return &extensibility;
    }

    PluginFormatLV2::PluginFormatLV2(std::vector<std::string> &overrideSearchPaths) {
        impl = new Impl(this);
    }

    PluginFormatLV2::~PluginFormatLV2() {
        delete impl;
    }

    PluginExtensibility<PluginFormat> * PluginFormatLV2::getExtensibility() {
        return impl->getExtensibility();
    }

    PluginScanning * PluginFormatLV2::scanning() {
        return impl->scanning();
    }

    std::vector<std::filesystem::path>& AudioPluginScannerLV2::getDefaultSearchPaths() {
        static std::filesystem::path defaultSearchPathsLV2[] = {
#if _WIN32
            std::string(getenv("APPDATA")) + "\\LV2",
            std::string(getenv("COMMONPROGRAMFILES")) + "\\LV2"
#elif __APPLE__
            std::string(getenv("HOME")) + "/Library/Audio/Plug-Ins/LV2",
            "/Library/Audio/Plug-Ins/LV2"
#else // We assume the rest covers Linux and other Unix-y platforms
            std::string(getenv("HOME")) + "/.lv2",
            "/usr/local/lib/lv2", // $PREFIX-based path
            "/usr/lib/lv2" // $PREFIX-based path
#endif
        };
        static std::vector<std::filesystem::path> ret = [] {
            std::vector<std::filesystem::path> paths{};
            for (auto& path : defaultSearchPathsLV2)
                paths.emplace_back(path);
            return paths;
        }();
        return ret;
    }

    void PluginFormatLV2::createInstance(PluginCatalogEntry* info,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) {
        impl->createInstance(info, callback);
    }

    PluginFormatLV2::Extensibility::Extensibility(PluginFormat &format) :
        PluginExtensibility(format) {
    }

    void PluginInstanceLV2::PluginStatesLV2::getState(std::vector<uint8_t> &state,
                                                      PluginStateSupport::StateContextType stateContextType,
                                                      bool includeUiState) {
        throw std::runtime_error("Not implemented");
    }

    void PluginInstanceLV2::PluginStatesLV2::setState(std::vector<uint8_t> &state,
                                                      PluginStateSupport::StateContextType stateContextType,
                                                      bool includeUiState) {
        throw std::runtime_error("Not implemented");
    }
}

