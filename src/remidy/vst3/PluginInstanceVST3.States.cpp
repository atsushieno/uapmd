#include "PluginFormatVST3.hpp"

namespace remidy {

    std::vector<uint8_t> PluginInstanceVST3::PluginStatesVST3::getState(PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
        std::vector<uint8_t> ret;
        VectorStream stream{ret};
        auto component = owner->component;
        component->vtable->component.get_state(component, (v3_bstream**) &stream);
        return ret;
    }

    void PluginInstanceVST3::PluginStatesVST3::setState(std::vector<uint8_t> &state,
                                                        PluginStateSupport::StateContextType stateContextType,
                                                        bool includeUiState) {
        VectorStream stream{state};
        auto component = owner->component;
        component->vtable->component.set_state(component, (v3_bstream**) &stream);
    }
}