#if __APPLE__

#include "PluginFormatAUv3.hpp"

remidy::PluginInstanceAUv3::PresetsSupport::PresetsSupport(remidy::PluginInstanceAUv3* owner) : owner(owner) {
    @autoreleasepool {
        if (owner->audioUnit == nil)
            return;

        NSArray<AUAudioUnitPreset*>* factoryPresets = [owner->audioUnit factoryPresets];
        if (factoryPresets == nil)
            return;

        items.reserve([factoryPresets count]);

        for (NSUInteger i = 0; i < [factoryPresets count]; i++) {
            AUAudioUnitPreset* preset = factoryPresets[i];

            // NOTE: some non-trivial replacement during AUv2->AUv3 migration
            // PresetInfo constructor: (id, name, bank, index)
            PresetInfo info(
                std::to_string([preset number]),
                std::string([[preset name] UTF8String]),
                0, // bank
                static_cast<int32_t>(i)
            );

            items.push_back(info);
        }
    }
}

int32_t remidy::PluginInstanceAUv3::PresetsSupport::getPresetIndexForId(std::string &id) {
    // NOTE: some non-trivial replacement during AUv2->AUv3 migration
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].id() == id)
            return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t remidy::PluginInstanceAUv3::PresetsSupport::getPresetCount() {
    return static_cast<int32_t>(items.size());
}

remidy::PresetInfo remidy::PluginInstanceAUv3::PresetsSupport::getPresetInfo(int32_t index) {
    if (index < 0 || index >= static_cast<int32_t>(items.size())) {
        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        return PresetInfo("", "", 0, -1);
    }
    return items[index];
}

void remidy::PluginInstanceAUv3::PresetsSupport::loadPreset(int32_t index) {
    @autoreleasepool {
        if (owner->audioUnit == nil) {
            owner->logger()->logError("%s: loadPreset - audioUnit is nil", owner->name.c_str());
            return;
        }

        if (index < 0 || index >= static_cast<int32_t>(items.size())) {
            owner->logger()->logError("%s: loadPreset - invalid index %d", owner->name.c_str(), index);
            return;
        }

        NSArray<AUAudioUnitPreset*>* factoryPresets = [owner->audioUnit factoryPresets];
        if (factoryPresets == nil || index >= [factoryPresets count]) {
            owner->logger()->logError("%s: loadPreset - no factory presets", owner->name.c_str());
            return;
        }

        AUAudioUnitPreset* preset = factoryPresets[index];
        [owner->audioUnit setCurrentPreset:preset];

        owner->logger()->logInfo("%s: Loaded preset %d: %s",
                                owner->name.c_str(),
                                index,
                                [[preset name] UTF8String]);
    }
}

#endif
