#if __APPLE__

#include "PluginFormatAU.hpp"
#include <stdexcept>
#include <choc/platform/choc_ObjectiveCHelpers.h>

using namespace remidy;

PluginInstanceAU::PresetsSupport::PresetsSupport(PluginInstanceAU* owner) : owner(owner) {
    auto impl = [&] {
    CFArrayRef presets = nullptr;
    UInt32 size = sizeof(CFArrayRef);
    auto err = AudioUnitGetProperty(owner->instance, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size);
    if (err != noErr)
        return;

    auto presetCount = CFArrayGetCount(presets);
    for (CFIndex i = 0; i < presetCount; i++) {
        auto preset = (const AUPreset *) CFArrayGetValueAtIndex(presets, i);
        auto nsPresetName = (__bridge NSString *) preset->presetName;
        auto presetName = choc::objc::getString(nsPresetName);

        std::string id = std::to_string(preset->presetNumber);
        items.emplace_back(id, presetName, 0, preset->presetNumber);
    }
    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::State)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();
}

int32_t PluginInstanceAU::PresetsSupport::getPresetIndexForId(std::string &id) {
    for (int32_t i = 0, n = (int32_t) items.size(); i < n; i++) {
        if (items[i].id() == id)
            return i;
    }
    return -1;
}

int32_t PluginInstanceAU::PresetsSupport::getPresetCount() {
    return (int32_t) items.size();
}

PresetInfo PluginInstanceAU::PresetsSupport::getPresetInfo(int32_t index) {
    return items[(size_t) index];
}

void PluginInstanceAU::PresetsSupport::loadPreset(int32_t index) {
    auto impl = [&] {
    auto& preset = items[(size_t) index];
    AUPreset auPreset{};
    auPreset.presetNumber = preset.index();
    auPreset.presetName = CFStringCreateWithCString(nullptr, preset.name().c_str(), kCFStringEncodingUTF8);

    auto status = AudioUnitSetProperty(owner->instance, kAudioUnitProperty_PresentPreset,
                                       kAudioUnitScope_Global, 0, &auPreset, sizeof(auPreset));
    if (status != noErr)
        owner->logger()->logWarning("%s: failed to load AU preset %s (%d). Status: %d",
                                    owner->name.c_str(), preset.name().c_str(), preset.index(), status);

    if (auPreset.presetName)
        CFRelease(auPreset.presetName);
    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::State)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();
}

#endif
