#pragma once

#include "../remidy.hpp"

namespace remidy {
    class PluginFormatLV2 : public PluginFormat {
    public:
        class Impl;

        class Extensibility : public PluginExtensibility<PluginFormat> {
        public:
            explicit Extensibility(PluginFormat& format);
        };

        explicit PluginFormatLV2(std::vector<std::string>& overrideSearchPaths);
        ~PluginFormatLV2() override;

        std::string name() override { return "LV2"; }
        PluginExtensibility<PluginFormat>* getExtensibility() override;
        PluginScanning* scanning() override;
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return PluginUIThreadRequirement::None; }
        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return false; }

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) override;

    private:
        Impl *impl;
    };
}
