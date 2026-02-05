#pragma once

#include <choc/javascript/choc_javascript.h>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "uapmd/uapmd.hpp"

namespace uapmd {

struct ParameterUpdate {
    int32_t parameterIndex;
    double value;
};

/**
 * UapmdJSRuntime encapsulates the QuickJS runtime and provides
 * a clean API for registering UAPMD-specific JavaScript functions.
 * This class is decoupled from GUI components and can be used in
 * any context that needs JavaScript execution with UAPMD APIs.
 */
class UapmdJSRuntime {
    choc::javascript::Context jsContext_;

    // Parameter update queue for JavaScript polling
    std::unordered_map<int32_t, std::vector<ParameterUpdate>> js_parameter_updates_;
    std::mutex js_parameter_mutex_;
    std::map<int32_t, EventListenerId> js_parameter_listener_ids_; // std::map for deterministic cleanup order

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

    /**
     * Register parameter update listener for an instance.
     * Should be called when an instance is added to a track.
     */
    void registerParameterListener(int32_t instanceId);

    /**
     * Unregister parameter update listener for an instance.
     * Should be called when an instance is removed.
     */
    void unregisterParameterListener(int32_t instanceId);

    /**
     * Register listeners for all currently active instances.
     * Called on initialization or when script editor starts.
     */
    void registerAllParameterListeners();

    /**
     * Unregister all parameter listeners.
     * Called on cleanup or when script editor stops.
     */
    void unregisterAllParameterListeners();

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
