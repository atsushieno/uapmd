#pragma once

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatCLAP : public PluginFormat {

        class Extensibility : public PluginExtensibility<PluginFormat> {
            bool report_not_implemented{false};
        public:
            explicit Extensibility(PluginFormat& format);
        };

    public:
        explicit PluginFormatCLAP(std::vector<std::string>& overrideSearchPaths);
        ~PluginFormatCLAP() override;

        std::string name() override { return "CLAP"; }
        PluginScanning* scanning() override;
        PluginExtensibility<PluginFormat>* getExtensibility() override;
        // FIXME: CLAP is not that strict. We need to check through the entire API and add other flags than `Parameter`.
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override { return PluginUIThreadRequirement::AllNonAudioOperation; }
        bool canOmitUiState() override { return false; }
        bool isStateStructured() override { return false; }

        void createInstance(PluginCatalogEntry* info,
                            PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
                            ) override;

        class Impl;
        Impl *impl;
    };
}
