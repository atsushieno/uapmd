#pragma once

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatLV2 : public PluginFormat {
    public:
        class Extensibility : public PluginExtensibility<PluginFormat> {
        public:
            explicit Extensibility(PluginFormat& format);
        };

        PluginFormatLV2() = default;
        ~PluginFormatLV2() override = default;

        std::string name() override { return "LV2"; }
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return PluginUIThreadRequirement::None; }
        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return false; }

        static std::unique_ptr<PluginFormatLV2> create(std::vector<std::string>& overrideSearchPaths);
    };
}
