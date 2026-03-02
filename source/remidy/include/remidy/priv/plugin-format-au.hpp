#pragma once

#if __APPLE__

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatAU : public PluginFormat {
    public:
        class Extensibility : public PluginExtensibility<PluginFormat> {
        public:
            explicit Extensibility(PluginFormat& format);
        };

        PluginFormatAU() = default;
        ~PluginFormatAU() override = default;

        std::string name() override { return "AU"; }
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return PluginUIThreadRequirement::None; }
        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return true; }

        static std::unique_ptr<PluginFormatAU> create();
    };
}

#endif
