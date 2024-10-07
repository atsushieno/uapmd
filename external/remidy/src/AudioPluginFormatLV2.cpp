#include "remidy.hpp"
#include <lilv/lilv.h>

namespace remidy {
    class AudioPluginFormatLV2::Impl {
        AudioPluginFormatLV2* owner;
        Logger* logger;
        Extensibility extensibility;

    public:
        explicit Impl(AudioPluginFormatLV2* owner) :
            owner(owner),
            logger(Logger::global()),
            extensibility(*owner) {
        }

        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility();
        PluginCatalog scanAllAvailablePlugins();
        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback);
        void unrefLibrary(PluginCatalogEntry *info);
        PluginCatalog createCatalogFragment(const std::filesystem::path &bundlePath);
    };

    PluginCatalog AudioPluginFormatLV2::Impl::scanAllAvailablePlugins() {
        // FIXME: implement
        throw std::runtime_error("AudioPluginFormatLV2::scanAllAvailablePlugins() is not implemented");
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