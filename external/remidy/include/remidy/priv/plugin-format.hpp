#pragma once

#include <future>
#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormat {
        class Impl;
        Impl *impl{};

    protected:
        AudioPluginFormat();
        virtual ~AudioPluginFormat() = default;

    public:
        virtual std::string name() = 0;

        virtual AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() { return nullptr; }

        bool hasPluginListCache();
        bool supportsGlobalIdentifier();
        // Indicates whether the plugin API requires sample rate at *instantiating*.
        // Only LV2 requires this, among VST3, AUv2/v3, LV2, and CLAP.
        // This impacts on whether we will have to discard and instantiate a plugin
        // when our use app changes the sample rate.
        bool instantiateRequiresSampleRate();
        PluginCatalog& getAvailablePlugins();

        virtual bool usePluginSearchPaths() = 0;
        virtual std::vector<std::string>& getDefaultSearchPaths() = 0;
        enum ScanningStrategyValue {
            NO,
            MAYBE,
            YES
        };
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;
        virtual PluginCatalog scanAllAvailablePlugins() = 0;

        virtual std::string savePluginInformation(PluginCatalogEntry* info) = 0;
        virtual std::string savePluginInformation(AudioPluginInstance* instance) = 0;
        virtual std::unique_ptr<PluginCatalogEntry> restorePluginInformation(std::string& data) = 0;

        struct InvokeResult {
            std::unique_ptr<AudioPluginInstance> instance;
            std::string error;
        };
        virtual void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) = 0;
    };

    class DesktopAudioPluginFormat : public AudioPluginFormat {
    protected:
        explicit DesktopAudioPluginFormat() = default;

        std::vector<std::string> overrideSearchPaths{};

        std::vector<std::string>& getOverrideSearchPaths() { return overrideSearchPaths; }
        void addSearchPath(const std::string& path) { overrideSearchPaths.emplace_back(path); }

        virtual PluginCatalog createCatalogFragment(std::filesystem::path &bundlePath) = 0;
    };
}