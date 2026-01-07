#pragma once

#include <choc/javascript/choc_javascript.h>
#include <string>

namespace uapmd {

/**
 * UapmdJSRuntime encapsulates the QuickJS runtime and provides
 * a clean API for registering UAPMD-specific JavaScript functions.
 * This class is decoupled from GUI components and can be used in
 * any context that needs JavaScript execution with UAPMD APIs.
 */
class UapmdJSRuntime {
    choc::javascript::Context jsContext_;

public:
    UapmdJSRuntime();

    /**
     * Returns the underlying choc::javascript::Context for advanced use cases.
     */
    choc::javascript::Context& context() { return jsContext_; }
    const choc::javascript::Context& context() const { return jsContext_; }

    /**
     * Re-initialize the JavaScript context (e.g., after a reset).
     */
    void reinitialize();

private:
    void registerConsoleFunctions();
    void registerPluginCatalogAPI();
    void registerPluginScanToolAPI();
    void registerPluginInstanceAPI();
    void registerSequencerMidiAPI();
    void registerSequencerTransportAPI();
    void registerSequencerInstanceAPI();
    void registerSequencerAudioAnalysisAPI();
    void registerSequencerAudioDeviceAPI();
};

} // namespace uapmd
