#pragma once

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatCLAP : public PluginFormat {
    public:
        class Extensibility : public PluginExtensibility<PluginFormat> {
            bool report_not_implemented{false};
        public:
            explicit Extensibility(PluginFormat& format);
        };

        PluginFormatCLAP() = default;
        ~PluginFormatCLAP() override = default;

        std::string name() override { return "CLAP"; }
        // FIXME: CLAP is not that strict. We need to check through the entire API and add other flags than `Parameter`.
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override { return PluginUIThreadRequirement::AllNonAudioOperation; }
        bool canOmitUiState() override { return false; }
        bool isStateStructured() override { return false; }

        static std::unique_ptr<PluginFormatCLAP> create(std::vector<std::string>& overrideSearchPaths);
    };
}
