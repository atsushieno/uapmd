
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
    auto params = dynamic_cast<remidy::PluginInstanceVST3::ParameterSupport*>(owner->parameters());
    if (!params) {
        Logger::global()->logError("ParameterSupport is not available");
        return;
    }

    auto programParamId = params->getProgramChangeParameterId();
    auto programParamIndex = params->getProgramChangeParameterIndex();

    if (programParamId == static_cast<ParamID>(-1) || programParamIndex == -1) {
        Logger::global()->logError("No program change parameter found - plugin may not support program lists correctly");
        return;
    }

    // Calculate the normalized value for the program parameter
    // The program parameter is typically normalized to [0.0, 1.0] across all available programs
    auto presetCount = getPresetCount();
    if (presetCount <= 0) {
        Logger::global()->logError("No presets available");
        return;
    }

    // Normalize index to [0.0, 1.0] range
    // JUCE does: value = program / max(1, programCount - 1)
    double normalizedValue = static_cast<double>(index) / static_cast<double>(std::max(1, presetCount - 1));

    // Set the parameter value through the controller
    // This is the normative VST3 way - the plugin handles the actual preset loading
    auto controller = owner->controller;
    auto result = controller->setParamNormalized(programParamId, normalizedValue);
    if (result != kResultOk) {
        Logger::global()->logError(std::format("Failed to set program parameter: result code: {}, index: {}, normalized: {}",
            result, index, normalizedValue).c_str());
        return;
    }

    // Queue the parameter change for the audio processor
    auto pvc = owner->processDataInputParameterChanges.asInterface();
    int32_t queueIndex = 0;
    auto queue = pvc->addParameterData(programParamId, queueIndex);
    if (queue) {
        int32_t pointIndex = 0;
        queue->addPoint(0, normalizedValue, pointIndex);
    }

    // Refresh parameter metadata and poll values after preset load
    // This handles plugins like Dexed that may change parameter ranges or not emit proper change notifications
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

