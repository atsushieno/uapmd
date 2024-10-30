#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatAU : public AudioPluginFormat {
        class Impl;
        Impl* impl;

    public:
        AudioPluginFormatAU();
        ~AudioPluginFormatAU() override;

        class Extensibility : public AudioPluginExtensibility<AudioPluginFormat> {
        public:
            explicit Extensibility(AudioPluginFormat& format);
        };

        Logger* getLogger();

        std::string name() override { return "AU"; }
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;

        AudioPluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return AudioPluginUIThreadRequirement::None; }

        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }

        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }

        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<AudioPluginInstance> instance, std::string error)>&& callback) override;
    };
}
