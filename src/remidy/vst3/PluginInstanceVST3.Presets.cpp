
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

remidy::PluginInstanceVST3::PresetsSupport::PresetsSupport(remidy::PluginInstanceVST3* owner) : owner(owner) {
    auto unitInfo = owner->unit_info;
    if (unitInfo != nullptr) {
        auto numBanks = unitInfo->getProgramListCount();
        for (int32_t b = 0; b < numBanks; b++) {
            std::vector<PresetInfo> bank{};
            ProgramListInfo list{};
            if (unitInfo->getProgramListInfo(b, list) != kResultOk)
                continue; // FIXME: should we simply ignore?
            for (int32_t p = 0; p < list.programCount; p++) {
                // FIXME: should we support program index >= 128 ?
                if (p >= 128)
                    continue;

                String128 name{};
                auto status = unitInfo->getProgramName(list.id, p, name);
                if (status != kResultOk)
                    continue; // FIXME: should we simply ignore?
                bank.emplace_back(std::move(PresetInfo{std::format("{}_{}", b, p), vst3StringToStdString(name), b, p}));
            }
            banks.push_back(bank);
        }
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
    auto unitInfo = owner->unit_info;
    auto states = owner->_states;

    ProgramListInfo list{};
    auto bank = index / 0x80;
    auto program = index % 0x80;
    auto result = unitInfo->getProgramListInfo(bank, list);
    if (result != kResultOk) {
        Logger::global()->logError(std::format("Could not retrieve preset bank {}: {}", bank, result).c_str());
        return; // FIXME: no error reporting?
    }

    /*
    String128 path{};
    result = unitInfo->getProgramInfo(list.id, program, PresetAttributes::kName, path);
    //result = unitInfo->getProgramInfo(list.id, prog, PresetAttributes::kFilePathStringType, path);
    if (result != kResultOk) {
        Logger::global()->logError(std::format("Could not retrieve preset bank {}: {}", bank, result).c_str());
    }
    std::cerr << "Loading preset " << index << " from " << vst3StringToStdString(path) << std::endl;
    */

    // copied from PluginInstanceVST3::ParameterSupport::setProgramChange()
    IBStream *stream;
    result = unitInfo->setUnitProgramData(list.id, program, stream);
    if (result != kResultOk) {
        std::cerr << std::format("Failed to set unit program data: result code: {}, bank: {}, program: {}", result, bank, program) << std::endl;
        return;
    }

    int64_t size;
    stream->seek(0, IBStream::kIBSeekEnd, &size);
    std::vector<uint8_t> buf(size);
    int32_t read;
    stream->read(buf.data(), size, &read);
    states->setState(buf, remidy::PluginStateSupport::StateContextType::Preset, true);

    // Refresh parameter metadata and poll values after preset load
    // This handles plugins like Dexed that may change parameter ranges or not emit proper change notifications
    auto params = dynamic_cast<remidy::PluginInstanceVST3::ParameterSupport*>(owner->parameters());
    if (params) {
        params->refreshAllParameterMetadata();
        auto& paramList = params->parameters();
        for (size_t i = 0; i < paramList.size(); i++) {
            double value;
            if (params->getParameter(static_cast<uint32_t>(i), &value) == remidy::StatusCode::OK) {
                auto paramId = params->getParameterId(static_cast<uint32_t>(i));
                params->notifyParameterValue(paramId, value);
            }
        }
    }
}

