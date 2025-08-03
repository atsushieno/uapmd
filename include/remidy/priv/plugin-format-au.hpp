#pragma once

#if __APPLE__

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatAU : public PluginFormat {
        class Impl;
        Impl* impl;

    public:
        PluginFormatAU();
        ~PluginFormatAU() override;

        class Extensibility : public PluginExtensibility<PluginFormat> {
        public:
            explicit Extensibility(PluginFormat& format);
        };

        std::string name() override { return "AU"; }
        PluginExtensibility<PluginFormat>* getExtensibility() override;
        PluginScanning* scanning() override;

        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return PluginUIThreadRequirement::None; }
        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return true; }

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) override;
    };
}

#endif
