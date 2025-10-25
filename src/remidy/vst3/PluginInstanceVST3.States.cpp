#include "PluginFormatVST3.hpp"

namespace remidy {

    std::vector<uint8_t> PluginInstanceVST3::PluginStatesVST3::getState(StateContextType stateContextType,
                                                        bool includeUiState) {
        std::vector<uint8_t> ret;
        VectorStream stream{ret};
        auto component = owner->component;
        component->getState((IBStream*) &stream);
        return ret;
    }

    void PluginInstanceVST3::PluginStatesVST3::setState(std::vector<uint8_t> &state,
                                                        StateContextType stateContextType,
                                                        bool includeUiState) {
        VectorStream stream{state};
        auto component = owner->component;
        component->setState((IBStream*) &stream);
    }
}