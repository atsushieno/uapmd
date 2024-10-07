#include "remidy.hpp"
#include <lilv/lilv.h>

namespace remidy {
    class AudioPluginFormatLV2::Impl {
        AudioPluginFormatLV2* owner;
        Logger* logger;
        Extensibility extensibility;
        LilvWorld *world;

    public:
        explicit Impl(AudioPluginFormatLV2* owner) :
            owner(owner),
            logger(Logger::global()),
            extensibility(*owner) {
            world = lilv_world_new();
            lilv_world_load_all(world);
        }
        ~Impl() {
            lilv_free(world);
        }

        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility();
        PluginCatalog scanAllAvailablePlugins();
        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback);
        void unrefLibrary(PluginCatalogEntry *info);
        PluginCatalog createCatalogFragment(const std::filesystem::path &bundlePath);
    };

    PluginCatalog AudioPluginFormatLV2::Impl::scanAllAvailablePlugins() {
        PluginCatalog ret{};

        auto plugins = lilv_world_get_all_plugins(world);
        LILV_FOREACH(plugins, iter, plugins) {
            const LilvPlugin* plugin = lilv_plugins_get(plugins, iter);
            auto entry = std::make_unique<PluginCatalogEntry>();
            auto uriNode = lilv_plugin_get_uri(plugin);
            std::string uri = lilv_node_as_uri(uriNode);
            auto bundleUriNode = lilv_plugin_get_bundle_uri(plugin);
            auto bundlePath = lilv_uri_to_path(lilv_node_as_uri(bundleUriNode));
            entry->bundlePath(std::filesystem::path{bundlePath});
            entry->pluginId(uri);
            auto nameNode = lilv_plugin_get_name(plugin);
            std::string name = lilv_node_as_string(nameNode);
            auto authorNameNode = lilv_plugin_get_author_name(plugin);
            auto authorName = lilv_node_as_string(authorNameNode);
            auto authorUrlNode = lilv_plugin_get_author_homepage(plugin);
            auto authorUrl = lilv_node_as_string(authorUrlNode);
            entry->setMetadataProperty(remidy::PluginCatalogEntry::DisplayName, name);
            entry->setMetadataProperty(remidy::PluginCatalogEntry::VendorName, authorName);
            entry->setMetadataProperty(remidy::PluginCatalogEntry::ProductUrl, authorUrl);
            ret.add(std::move(entry));
        }
        return ret;
    }

    void AudioPluginFormatLV2::Impl::createInstance(PluginCatalogEntry *info,
        std::function<void(InvokeResult)> callback) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::createInstance() is not implemented");
    }

    void AudioPluginFormatLV2::Impl::unrefLibrary(PluginCatalogEntry *info) {
    }

    PluginCatalog AudioPluginFormatLV2::Impl::createCatalogFragment(const std::filesystem::path &bundlePath) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::createCatalogFragment() is not implemented");
    }

    AudioPluginExtensibility<AudioPluginFormat> * AudioPluginFormatLV2::Impl::getExtensibility() {
        return &extensibility;
    }

    AudioPluginFormatLV2::AudioPluginFormatLV2(std::vector<std::string> &overrideSearchPaths) {
        impl = new Impl(this);
    }

    AudioPluginFormatLV2::~AudioPluginFormatLV2() {
        delete impl;
    }

    AudioPluginExtensibility<AudioPluginFormat> * AudioPluginFormatLV2::getExtensibility() {
        return impl->getExtensibility();
    }

    std::vector<std::string> & AudioPluginFormatLV2::getDefaultSearchPaths() {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::getDefaultSearchPaths() is not implemented");
    }

    PluginCatalog AudioPluginFormatLV2::scanAllAvailablePlugins() {
        return impl->scanAllAvailablePlugins();
    }

    std::string AudioPluginFormatLV2::savePluginInformation(PluginCatalogEntry *identifier) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::savePluginInformation() is not implemented");
    }

    std::string AudioPluginFormatLV2::savePluginInformation(AudioPluginInstance *instance) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::savePluginInformation() is not implemented");
    }

    std::unique_ptr<PluginCatalogEntry> AudioPluginFormatLV2::restorePluginInformation(std::string &data) {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::restorePluginInformation() is not implemented");
    }

    void AudioPluginFormatLV2::createInstance(PluginCatalogEntry *info,
        std::function<void(InvokeResult)> callback) {
        impl->createInstance(info, callback);
    }

    PluginCatalog AudioPluginFormatLV2::createCatalogFragment(std::filesystem::path &bundlePath) {
        return impl->createCatalogFragment(bundlePath);
    }

    AudioPluginFormatLV2::Extensibility::Extensibility(AudioPluginFormat &format) :
        AudioPluginExtensibility(format) {
    }
}