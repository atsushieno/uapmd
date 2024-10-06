#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatAU : public AudioPluginFormat {
        class Impl;
        Impl* impl;

    public:
        AudioPluginFormatAU();
        ~AudioPluginFormatAU() override;

        virtual std::string name() { return "AU"; }
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;

        bool usePluginSearchPaths() override { return false; }

        std::vector<std::string> & getDefaultSearchPaths() override;

        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NO; }

        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NO; }

        PluginCatalog scanAllAvailablePlugins() override;

        std::string savePluginInformation(PluginCatalogEntry *info) override;

        std::string savePluginInformation(AudioPluginInstance *instance) override;

        std::unique_ptr<PluginCatalogEntry> restorePluginInformation(std::string &data) override;

        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) override;
    };
}
