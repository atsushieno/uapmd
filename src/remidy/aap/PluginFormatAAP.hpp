#pragma once

#include "remidy.hpp"
#include <aap/core/plugin-information.h>

namespace remidy {
    class PluginFormatAAPImpl;

    class PluginScanningAAP : public PluginScanning {
        PluginFormatAAPImpl* owner;

    public:
        PluginScanningAAP(PluginFormatAAPImpl* owner);
        ~PluginScanningAAP() override = default;

        ScanningStrategyValue scanRequiresLoadLibrary() override;

        ScanningStrategyValue scanRequiresInstantiation() override;

        std::vector<std::unique_ptr<PluginCatalogEntry>>
        scanAllAvailablePlugins(bool requireFastScanning) override;
    };

    class PluginInstanceAAP : public PluginInstance {
    public:
        PluginUIThreadRequirement requiresUIThreadOn() override;

        StatusCode configure(ConfigurationRequest &configuration) override;

        StatusCode startProcessing() override;

        StatusCode stopProcessing() override;

        StatusCode process(AudioProcessContext &process) override;

        PluginAudioBuses *audioBuses() override;

        PluginParameterSupport *parameters() override;

        PluginStateSupport *states() override;

        PluginPresetsSupport *presets() override;

        PluginUISupport *ui() override;

    };

    class PluginFormatAAPImpl : public PluginFormatAAP {
        PluginScanningAAP scanning_{this};
    public:
        PluginFormatAAPImpl();

        PluginScanning *scanning() override { return &scanning_; }

        void createInstance(PluginCatalogEntry *info, PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance>,
                                               std::string)> callback) override;

    };
}
