#pragma once

#if __APPLE__

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatAUv3 : public PluginFormat {
    public:
        class Extensibility : public PluginExtensibility<PluginFormat> {
        public:
            explicit Extensibility(PluginFormat& format);
        };

        PluginFormatAUv3() = default;
        ~PluginFormatAUv3() override = default;

        std::string name() override { return "AU"; }
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return PluginUIThreadRequirement::None; }
        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return true; }

        static std::unique_ptr<PluginFormatAUv3> create();
    };
}

#endif
