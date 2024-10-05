#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatVST3 : public DesktopAudioPluginFormat {
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

        explicit AudioPluginFormatVST3(std::vector<std::string>& overrideSearchPaths);
        ~AudioPluginFormatVST3() override;

        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;
        bool usePluginSearchPaths() override;
        std::vector<std::string>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        PluginCatalog scanAllAvailablePlugins() override;

        std::string savePluginInformation(PluginCatalogEntry* identifier) override;
        std::string savePluginInformation(AudioPluginInstance* instance) override;
        std::unique_ptr<PluginCatalogEntry> restorePluginInformation(std::string& data) override;

        AudioPluginInstance* createInstance(PluginCatalogEntry* uniqueId) override;

        PluginCatalog createCatalogFragment(std::filesystem::path &bundlePath) override;

    private:
        Impl *impl;
    };
}
