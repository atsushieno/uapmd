#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "../remidy.hpp"

namespace remidy {

#if defined(__EMSCRIPTEN__)

    class PluginFormatWebCLAP : public PluginFormat {
    public:
        static std::unique_ptr<PluginFormatWebCLAP> create();

        PluginFormatWebCLAP();
        ~PluginFormatWebCLAP() override = default;

        std::string name() override;
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry* entry) override;
        bool canOmitUiState() override;
        bool isStateStructured() override;
        PluginScanning* scanning() override;
        void createInstance(PluginCatalogEntry* info,
                            PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) override;

    private:
        class Scanner : public PluginScanning {
            PluginFormatWebCLAP& owner;
        public:
            explicit Scanner(PluginFormatWebCLAP& owner);
            ScanningStrategyValue scanRequiresLoadLibrary() override;
            ScanningStrategyValue scanRequiresInstantiation() override;
            std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins(bool requireFastScanning) override;
        };

        Scanner scanner_;
    };

#else

    class PluginFormatWebCLAP : public PluginFormat {
        class ScannerStub : public PluginScanning {
        public:
            ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
            ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }
            std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins(bool) override {
                return {};
            }
        };

        ScannerStub scanner_;
    public:
        static std::unique_ptr<PluginFormatWebCLAP> create() {
            return std::make_unique<PluginFormatWebCLAP>();
        }

        PluginFormatWebCLAP() = default;
        ~PluginFormatWebCLAP() override = default;

        std::string name() override { return "WebCLAP"; }
        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override {
            return PluginUIThreadRequirement::None;
        }
        bool canOmitUiState() override { return false; }
        bool isStateStructured() override { return false; }
        PluginScanning* scanning() override { return &scanner_; }
        void createInstance(PluginCatalogEntry*,
                            PluginInstantiationOptions,
                            std::function<void(std::unique_ptr<PluginInstance>, std::string)> callback) override {
            callback(nullptr, "WebCLAP runtime integration is only available in the browser build.");
        }
    };

#endif
}
