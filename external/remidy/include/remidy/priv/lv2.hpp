#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatLV2 : public DesktopAudioPluginFormat {
    public:
        class Impl;

        class Extensibility : public AudioPluginExtensibility<AudioPluginFormat> {
            bool report_not_implemented{false};
        public:
            explicit Extensibility(AudioPluginFormat& format);

            bool reportNotImplemented() { return report_not_implemented; }
            StatusCode reportNotImplemented(bool newValue) {
                report_not_implemented = newValue;
                return StatusCode::OK;
            }
        };

        explicit AudioPluginFormatLV2(std::vector<std::string>& overrideSearchPaths);
        ~AudioPluginFormatLV2() override;

        virtual std::string name() { return "LV2"; }
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;
        bool usePluginSearchPaths() { return true; }
        std::vector<std::string>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() { return ScanningStrategyValue::NO; }
        ScanningStrategyValue scanRequiresInstantiation() { return ScanningStrategyValue::NO; }
        PluginCatalog scanAllAvailablePlugins() override;

        std::string savePluginInformation(PluginCatalogEntry* identifier) override;
        std::string savePluginInformation(AudioPluginInstance* instance) override;
        std::unique_ptr<PluginCatalogEntry> restorePluginInformation(std::string& data) override;

        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) override;

        PluginCatalog createCatalogFragment(std::filesystem::path &bundlePath) override;

    private:
        Impl *impl;
    };
}
