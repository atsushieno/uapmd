#include <cstdint>
#include "remidy/remidy.hpp"
#include <aap/plugin-meta-info.h>
#include "PluginFormatAAP.hpp"

std::vector<uint8_t>
remidy::PluginInstanceAAP::StateSupport::getState(StateContextType stateContextType,
                                                  bool includeUiState) {
    auto stateObj = owner->aapInstance()->getStandardExtensions().getState();
    std::vector<uint8_t> ret{};
    ret.resize(stateObj.data_size);
    if (stateObj.data_size > 0)
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

void remidy::PluginInstanceAAP::StateSupport::requestState(
        StateContextType stateContextType,
        bool includeUiState,
        void* callbackContext,
        std::function<void(std::vector<uint8_t> state, std::string error, void* callbackContext)> receiver) {
    queue_.enqueueRequest(callbackContext, std::move(receiver),
                          [this, stateContextType, includeUiState](std::function<bool()> isCancelled,
                                                                    std::function<void(std::vector<uint8_t> state, std::string error)> finish) mutable {
                              if (isCancelled()) {
                                  finish({}, "instance destroyed");
                                  return;
                              }
                              finish(getState(stateContextType, includeUiState), "");
                          });
}

void remidy::PluginInstanceAAP::StateSupport::loadState(
        std::vector<uint8_t> state,
        StateContextType stateContextType,
        bool includeUiState,
        void* callbackContext,
        std::function<void(std::string error, void* callbackContext)> completed) {
    queue_.enqueueLoad(callbackContext, std::move(completed),
                       [this, state = std::move(state), stateContextType, includeUiState](std::function<bool()> isCancelled,
                                                                                           std::function<void(std::string error)> finish) mutable {
                           if (isCancelled()) {
                               finish("instance destroyed");
                               return;
                           }
                           setState(state, stateContextType, includeUiState);
                           finish("");
                       });
}
