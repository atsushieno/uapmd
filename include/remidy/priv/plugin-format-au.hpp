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
        AudioPluginScanner* scanner() override;

        AudioPluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return AudioPluginUIThreadRequirement::None; }

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<AudioPluginInstance> instance, std::string error)>&& callback) override;
    };
}
