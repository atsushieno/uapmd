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

    // What a host needs from a WebCLAP plugin instance beyond PluginInstance.
    //
    // A WebCLAP plugin lives partly in the browser, so two things a host does for other
    // formats have to be routed across that boundary instead: delivering input events, and
    // telling the plugin where it sits in the audio graph. The concrete instance class is
    // private to the format's implementation, so this is the part hosts can reach --
    // `dynamic_cast` a `PluginInstance*` to it and act if it answers.
    class PluginInstanceWebCLAPControl {
    public:
        virtual ~PluginInstanceWebCLAPControl() = default;

        // Sends UMP input to the browser-side plugin for the current cycle.
        virtual bool sendUmpInputEvents(const uint32_t* events, size_t sizeInBytes) = 0;

        // Tells the plugin which track's graph it belongs to, and in what order, so the
        // browser side can wire its audio node up to match.
        virtual void attachToTrackGraph(int32_t trackIndex, bool isMasterTrack, uint32_t order) = 0;
    };

} // namespace remidy

#endif // __EMSCRIPTEN__
