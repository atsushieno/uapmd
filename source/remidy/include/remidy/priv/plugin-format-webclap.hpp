#pragma once

#ifdef __EMSCRIPTEN__

#include "plugin-format.hpp"
#include <memory>

namespace remidy {

    // Public base class for the WebCLAP plugin format (Emscripten / browser only).
    //
    // WebCLAP plugins are CLAP plugins compiled to WebAssembly and distributed as
    // .wclap.tar.gz archives.  Audio processing runs on the AudioWorklet thread via
    // wclap.mjs; this C++ class manages the lifecycle and control-plane messaging.
    class PluginFormatWebCLAP : public PluginFormat {
    public:
        PluginFormatWebCLAP() = default;
        ~PluginFormatWebCLAP() override = default;

        std::string name() override { return "WebCLAP"; }

        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override {
            return PluginUIThreadRequirement::None;
        }

        bool canOmitUiState() override { return true; }
        bool isStateStructured() override { return false; }

        // Dispatches an incoming JSON message from the browser-side bridge to the
        // pending createInstance callbacks and live instances. Called by
        // uapmd_webclap_on_worklet_message(), whose exported symbol name is kept
        // stable for the engine layer.
        virtual void onBridgeMessage(const char* json) = 0;

        static std::unique_ptr<PluginFormatWebCLAP> create();
    };

} // namespace remidy

#endif // __EMSCRIPTEN__
