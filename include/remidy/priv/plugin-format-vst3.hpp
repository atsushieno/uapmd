#pragma once

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatVST3 : public PluginFormat {
    public:
        class Impl;

        class Extensibility : public PluginExtensibility<PluginFormat> {
            bool report_not_implemented{false};
        public:
            explicit Extensibility(PluginFormat& format);

            bool reportNotImplemented() { return report_not_implemented; }
            StatusCode reportNotImplemented(bool newValue) {
                report_not_implemented = newValue;
                return StatusCode::OK;
            }
        };

        explicit PluginFormatVST3(std::vector<std::string>& overrideSearchPaths);
        ~PluginFormatVST3() override;

        std::string name() override { return "VST3"; }
        PluginScanning* scanning() override;
        PluginExtensibility<PluginFormat>* getExtensibility() override;
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override { return PluginUIThreadRequirement::AllNonAudioOperation; }
        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return false; }

        void createInstance(PluginCatalogEntry* info,
                            PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
                            ) override;

    private:
        Impl *impl;
    };
}
