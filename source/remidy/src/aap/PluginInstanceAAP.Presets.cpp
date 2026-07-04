#include <cstdint>
#include <future>
#include "remidy/remidy.hpp"
#include <aap/plugin-meta-info.h>
#include "PluginFormatAAP.hpp"

int32_t remidy::PluginInstanceAAP::PresetsSupport::getPresetCount() {
    return owner->aapInstance()->getStandardExtensions().getPresetCount();
}

remidy::PresetInfo remidy::PluginInstanceAAP::PresetsSupport::getPresetInfo(int32_t index) {
    auto promise = std::make_shared<std::promise<aap::Result<aap_preset_t>>>();
    auto future = promise->get_future();

    owner->aapInstance()->getStandardExtensions().getPresetAsync(index, [promise](aap::Result<aap_preset_t> result) {
        promise->set_value(std::move(result));
    });

    auto result = future.get();
    if (!result.error.empty()) {
        Logger::global()->logWarning("AAP: failed to get preset %d: %s", index, result.error.c_str());
        return PresetInfo{"", "", 0, -1};
    }

    return PresetInfo{std::to_string(result.value.id), result.value.name, 0, index};
}

void remidy::PluginInstanceAAP::PresetsSupport::loadPreset(int32_t index) {
    auto promise = std::make_shared<std::promise<aap::Result<bool>>>();
    auto future = promise->get_future();

    owner->aapInstance()->getStandardExtensions().setPresetIndexAsync(index, [promise](aap::Result<bool> result) {
        promise->set_value(std::move(result));
    });

    auto result = future.get();
    if (!result.error.empty())
        Logger::global()->logWarning("AAP: failed to load preset %d: %s", index, result.error.c_str());
}

void remidy::PluginInstanceAAP::PresetsSupport::loadPreset(int32_t index, std::function<void(std::string error, void* callbackContext)> completed) {
    owner->aapInstance()->getStandardExtensions().setPresetIndexAsync(index, [completed = std::move(completed)](aap::Result<bool> result) mutable {
        if (completed)
            completed(std::move(result.error), nullptr);
    });
}
