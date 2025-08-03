#pragma once

#include <string>
#include <vector>
#include <ranges>

namespace remidy {

    class PluginStateSupport {
    public:
        // usable only in LV2 and CLAP. VST3 and AU has no concept for them.
        enum class StateContextType {
            Remember, // LV2_STATE_IS_NATIVE
            Copyable, // LV2_STATE_IS_POD
            Preset,
            Project,  // LV2_STATE_IS_PORTABLE?
        };

        virtual ~PluginStateSupport() = default;

        virtual void getState(std::vector<uint8_t>& state, void* statePartId, StateContextType stateContextType, bool includeUiState) = 0;
        virtual void setState(std::vector<uint8_t>& state, void* statePartId, StateContextType stateContextType, bool includeUiState) = 0;
    };

}
