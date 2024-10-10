#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatAU : public AudioPluginFormat {
        class Impl;
        Impl* impl;

    public:
        AudioPluginFormatAU();
        ~AudioPluginFormatAU() override;

        virtual std::string name() override { return "AU"; }
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;

        bool usePluginSearchPaths() override { return false; }

        std::vector<std::filesystem::path> & getDefaultSearchPaths() override;

        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NO; }

        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NO; }

        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;

        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) override;
    };
}
