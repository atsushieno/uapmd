#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatLV2 : public AudioPluginFormat {
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
        AudioPluginScanner* scanner() override;
        AudioPluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return AudioPluginUIThreadRequirement::None; }

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<AudioPluginInstance> instance, std::string error)>&& callback) override;

    private:
        Impl *impl;
    };
}
