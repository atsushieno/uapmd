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

        std::string name() override { return "VST3"; }
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;
        AudioPluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override { return AllNonAudioOperation; }
        bool usePluginSearchPaths() override;
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;

        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) override;

    private:
        Impl *impl;
    };
}
