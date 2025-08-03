#include "PluginFormatLV2.hpp"

namespace remidy {
    LV2_State_Flags remidy_state_context_type_to_lv2_state_flags(PluginStateSupport::StateContextType stateContextType) {
        switch (stateContextType) {
            case PluginStateSupport::StateContextType::Remember:
                return LV2_STATE_IS_NATIVE;
            case PluginStateSupport::StateContextType::Copyable:
                return LV2_STATE_IS_POD;
            case PluginStateSupport::StateContextType::Preset:
            case PluginStateSupport::StateContextType::Project:
                return LV2_STATE_IS_PORTABLE;
        }
    }

    std::vector<uint8_t> PluginInstanceLV2::PluginStatesLV2::getState(
            PluginStateSupport::StateContextType stateContextType, bool includeUiState) {
        auto formatImpl = owner->formatImpl;
        auto& implContext = owner->implContext;
        auto plugin = owner->plugin;

        auto flags = remidy_state_context_type_to_lv2_state_flags(stateContextType);
        // should we pass `get_value` function here...?
        auto lilvState = lilv_state_new_from_instance(plugin, owner->instance, owner->getLV2UridMapData(),
                                     nullptr, nullptr, nullptr, nullptr,
                                     nullptr, this, flags, formatImpl->features.data());
        auto s = lilv_state_to_string(implContext.world, owner->getLV2UridMapData(), owner->getLV2UridUnmapData(),
                                      lilvState, nullptr, nullptr);
        auto size = strlen(s);
        std::vector<uint8_t> ret{};
        ret.resize(size);
        memcpy(ret.data(), s, size);
        free(s);
        return ret;
    }

    void PluginInstanceLV2::PluginStatesLV2::setState(std::vector<uint8_t> &state,
                                                      PluginStateSupport::StateContextType stateContextType,
                                                      bool includeUiState) {
        auto formatImpl = owner->formatImpl;

        auto flags = remidy_state_context_type_to_lv2_state_flags(stateContextType);
        auto lilvState = lilv_state_new_from_string(formatImpl->world, owner->getLV2UridMapData(),
                                                    (const char*) state.data());
        // should we pass `set_value` function here...?
        lilv_state_restore(lilvState, owner->instance, nullptr, this,flags, formatImpl->features.data());
    }
}
