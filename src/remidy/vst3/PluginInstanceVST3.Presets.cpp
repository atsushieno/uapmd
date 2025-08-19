
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

remidy::PluginInstanceVST3::PresetsSupport::PresetsSupport(remidy::PluginInstanceVST3* owner) : owner(owner) {
    auto unitInfo = owner->unit_info->vtable->unit_info;
    auto numBanks = unitInfo.get_program_list_count(owner->unit_info);
    for (int32_t b = 0; b < numBanks; b++) {
        std::vector<PresetInfo> bank{};
        v3_program_list_info list{};
        if (unitInfo.get_program_list_info(owner->unit_info, b, &list) != V3_OK)
            continue; // FIXME: should we simply ignore?
        for (int32_t p = 0; p < list.programCount; p++) {
            // FIXME: should we support program index >= 128 ?
            if (p >= 128)
                continue;

            v3_str_128 name;
            if (unitInfo.get_program_name(owner->unit_info, b, p, name) != V3_OK)
                continue; // FIXME: should we simply ignore?
            bank.emplace_back(std::move(PresetInfo{std::format("{}_{}", b, p), vst3StringToStdString(name), b, p}));
        }
        banks.push_back(bank);
    }
}

int32_t remidy::PluginInstanceVST3::PresetsSupport::getPresetIndexForId(std::string &id) {
    // id = "{bank}_{index}"
    auto idx = id.find('_');
    if (idx == std::string::npos)
        return std::stoi(id);
    return (std::stoi(id.substr(0, idx)) << 7) + std::stoi(id.substr(idx + 1));
}

int32_t remidy::PluginInstanceVST3::PresetsSupport::getPresetCount() {
    size_t ret = 0;
    for (auto& bank : banks)
        ret += bank.size();
    return (int32_t) ret;
}

remidy::PresetInfo remidy::PluginInstanceVST3::PresetsSupport::getPresetInfo(int32_t index) {
    return banks[index / 128][index % 128];
}

void remidy::PluginInstanceVST3::PresetsSupport::loadPreset(int32_t index) {

}

