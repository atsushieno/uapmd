#include "PluginFormatVST3.hpp"

namespace remidy {

    void PluginInstanceVST3::PluginStatesVST3::getState(std::vector<uint8_t> &state,
                                                        PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
        VectorStream stream{state};
        auto component = owner->component;
        component->vtable->component.get_state(component, (v3_bstream**) &stream);
    }

    void PluginInstanceVST3::PluginStatesVST3::setState(std::vector<uint8_t> &state,
                                                        PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
        VectorStream stream{state};
        auto component = owner->component;
        component->vtable->component.set_state(component, (v3_bstream**) &stream);
    }
}