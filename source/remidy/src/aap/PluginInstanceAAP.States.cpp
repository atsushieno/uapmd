#include <cstdint>
#include "remidy.hpp"
#include <aap/plugin-meta-info.h>
#include "PluginFormatAAP.hpp"

std::vector<uint8_t>
remidy::PluginInstanceAAP::StateSupport::getState(StateContextType stateContextType,
                                                  bool includeUiState) {
    auto stateObj = owner->aapInstance()->getStandardExtensions().getState();
    std::vector<uint8_t> ret{};
    memcpy(ret.data(), stateObj.data, stateObj.data_size);
    return ret;
}

void remidy::PluginInstanceAAP::StateSupport::setState(std::vector<uint8_t>& state,
                                                       StateContextType stateContextType,
                                                       bool includeUiState) {
    aap_state_t stateObj;
    stateObj.data = state.data();
    stateObj.data_size = state.size();
    // AAP does not support stateContextType and includeUiState.
    owner->aapInstance()->getStandardExtensions().setState(stateObj);
}
