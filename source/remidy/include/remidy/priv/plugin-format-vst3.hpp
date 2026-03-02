#pragma once

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatVST3 : public PluginFormat {
    public:
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

        PluginFormatVST3() = default;
        ~PluginFormatVST3() override = default;

        std::string name() override { return "VST3"; }
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override { return PluginUIThreadRequirement::AllNonAudioOperation; }
        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return false; }

        static std::unique_ptr<PluginFormatVST3> create(std::vector<std::string>& overrideSearchPaths);
    };
}
