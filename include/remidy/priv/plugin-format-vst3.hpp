#pragma once

#include "../remidy.hpp"

namespace remidy {
    class AudioPluginFormatVST3 : public AudioPluginFormat {
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
        AudioPluginScanner* scanner() override;
        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility() override;
        AudioPluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override { return AudioPluginUIThreadRequirement::AllNonAudioOperation; }

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<AudioPluginInstance> instance, std::string error)>&& callback) override;

    private:
        Impl *impl;
    };
}
