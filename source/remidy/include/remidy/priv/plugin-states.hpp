#pragma once

#include <functional>
#include <string>
#include <vector>

namespace remidy {

    class PluginStateSupport {
    public:
        // usable only in LV2 and CLAP. VST3 and AU have no concept for them.
        enum class StateContextType {
            Remember, // LV2_STATE_IS_NATIVE
            Copyable, // LV2_STATE_IS_POD
            Preset,
            Project,  // LV2_STATE_IS_PORTABLE?
        };

        virtual ~PluginStateSupport() = default;

        // Returns true if the plugin format requires state manipulation on the main thread.
        // `true` for VST3 and CLAP, and WebCLAP. `false` for AU, LV2, and AAP.
        virtual bool requiresMainThread() = 0;

        // OBSOLETE: use `requestState()` with appropriate callback instead.
        virtual std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) = 0;
        // OBSOLETE: use `loadState()` with appropriate callback instead.
        virtual void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) = 0;

        // Asynchronously get the current state. The callback is invoked exactly once.
        // Success is indicated by an empty error string; empty state data is valid.
        virtual void requestState(
                StateContextType stateContextType,
                bool includeUiState,
                void* callbackContext,
                std::function<void(std::vector<uint8_t> state, std::string error, void* callbackContext)> receiver) = 0;
        // Asynchronously set the current state. The callback is invoked exactly once.
        // Success is indicated by an empty error string.
        virtual void loadState(
                std::vector<uint8_t> state,
                StateContextType stateContextType,
                bool includeUiState,
                void* callbackContext,
                std::function<void(std::string error, void* callbackContext)> completed) = 0;
    };

}
