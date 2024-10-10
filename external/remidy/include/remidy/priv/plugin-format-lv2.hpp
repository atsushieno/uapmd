#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatLV2 : public DesktopAudioPluginFormat {
    public:
        class Impl;

        class Extensibility : public AudioPluginExtensibility<AudioPluginFormat> {
        public:
            explicit Extensibility(AudioPluginFormat& format);
        };

        explicit AudioPluginFormatLV2(std::vector<std::string>& overrideSearchPaths);
        ~AudioPluginFormatLV2() override;

        virtual std::string name() override { return "LV2"; }
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;
        bool usePluginSearchPaths() override { return true; }
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NO; }
        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NO; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;

        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) override;

        PluginCatalog createCatalogFragment(std::filesystem::path &bundlePath) override;

    private:
        Impl *impl;
    };
}
