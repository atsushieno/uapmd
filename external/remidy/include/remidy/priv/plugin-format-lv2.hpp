#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatLV2 : public FileBasedAudioPluginFormat {
    public:
        class Impl;

        class Extensibility : public AudioPluginExtensibility<AudioPluginFormat> {
        public:
            explicit Extensibility(AudioPluginFormat& format);
        };

        explicit AudioPluginFormatLV2(std::vector<std::string>& overrideSearchPaths);
        ~AudioPluginFormatLV2() override;

        std::string name() override { return "LV2"; }
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;
        AudioPluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override { return None; }
        bool usePluginSearchPaths() override { return true; }
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;

        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) override;

    private:
        Impl *impl;
    };
}
