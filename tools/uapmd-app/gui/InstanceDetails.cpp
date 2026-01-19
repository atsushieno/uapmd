#include "InstanceDetails.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <string>
#include <unordered_set>

#include <imgui.h>
#include <remidy-gui/remidy-gui.hpp>

#include "../AppModel.hpp"

namespace {
using ParameterContext = uapmd::gui::ParameterList::ParameterContext;

struct PerNoteSelection {
    remidy::PerNoteControllerContextTypes type{remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_NONE};
    remidy::PerNoteControllerContext context{};
};

std::optional<PerNoteSelection> buildPerNoteSelection(const uapmd::gui::ParameterList& list) {
    PerNoteSelection selection{};
    selection.context = remidy::PerNoteControllerContext{};
    switch (list.context()) {
    case ParameterContext::Global:
        return std::nullopt;
    case ParameterContext::Group:
        selection.type = remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_GROUP;
        selection.context.group = list.contextValue();
        break;
    case ParameterContext::Channel:
        selection.type = remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_CHANNEL;
        selection.context.channel = list.contextValue();
        break;
    case ParameterContext::Key:
        selection.type = remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE;
        selection.context.note = list.contextValue();
        break;
    }
    return selection;
}

std::vector<uapmd::ParameterMetadata> toParameterMetadata(const std::vector<remidy::PluginParameter*>& pluginParams) {
    std::vector<uapmd::ParameterMetadata> metadata;
    metadata.reserve(pluginParams.size());
    for (auto* param : pluginParams) {
        if (!param) {
            continue;
        }
        std::vector<uapmd::ParameterNamedValue> namedValues;
        namedValues.reserve(param->enums().size());
        for (const auto& enumeration : param->enums()) {
            namedValues.push_back(uapmd::ParameterNamedValue{
                .value = enumeration.value,
                .name = enumeration.label
            });
        }
        metadata.push_back(uapmd::ParameterMetadata{
            .index = param->index(),
            .stableId = param->stableId(),
            .name = param->name(),
            .path = param->path(),
            .defaultPlainValue = param->defaultPlainValue(),
            .minPlainValue = param->minPlainValue(),
            .maxPlainValue = param->maxPlainValue(),
            .automatable = param->automatable(),
            .hidden = param->hidden(),
            .discrete = param->discrete(),
            .namedValues = std::move(namedValues)
        });
    }
    return metadata;
}
}

namespace uapmd::gui {

void InstanceDetails::showWindow(int32_t instanceId) {
    auto it = windows_.find(instanceId);
    if (it == windows_.end()) {
        DetailsWindowState state;

        state.midiKeyboard.setOctaveRange(3, 4);
        state.midiKeyboard.setKeyEventCallback([this, instanceId](int note, int, bool isPressed) {
            auto& seq = uapmd::AppModel::instance().sequencer();
            if (isPressed) {
                seq.engine()->sendNoteOn(instanceId, note);
            } else {
                seq.engine()->sendNoteOff(instanceId, note);
            }
        });

        state.parameterList.setOnParameterChanged([this, instanceId](uint32_t parameterIndex, float value) {
            auto& seq = uapmd::AppModel::instance().sequencer();
            auto perNoteSelection = [this, instanceId]() -> std::optional<PerNoteSelection> {
                auto windowIt = windows_.find(instanceId);
                if (windowIt == windows_.end()) {
                    return std::nullopt;
                }
                return buildPerNoteSelection(windowIt->second.parameterList);
            }();

            if (!perNoteSelection) {
                seq.engine()->setParameterValue(instanceId, parameterIndex, value);
                return;
            }

            auto* pal = seq.engine()->getPluginInstance(instanceId);
            if (!pal) {
                return;
            }
            if (perNoteSelection->type == remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE) {
                pal->setPerNoteControllerValue(
                    static_cast<uint8_t>(perNoteSelection->context.note),
                    static_cast<uint8_t>(parameterIndex),
                    value);
                return;
            }
            if (auto* parameterSupport = pal->parameterSupport())
                parameterSupport->setPerNoteController(perNoteSelection->context, parameterIndex, value, 0);
        });

        state.parameterList.setOnGetParameterValueString([this, instanceId](uint32_t parameterIndex, float value) -> std::string {
            auto& seq = uapmd::AppModel::instance().sequencer();
            auto* pal = seq.engine()->getPluginInstance(instanceId);
            if (pal) {
                auto perNoteSelection = [this, instanceId]() -> std::optional<PerNoteSelection> {
                    auto windowIt = windows_.find(instanceId);
                    if (windowIt == windows_.end())
                        return std::nullopt;
                    return buildPerNoteSelection(windowIt->second.parameterList);
                }();

                if (perNoteSelection) {
                    return pal->getPerNoteControllerValueString(
                        static_cast<uint8_t>(perNoteSelection->context.note),
                        static_cast<uint8_t>(parameterIndex),
                        value);
                }
                return pal->getParameterValueString(parameterIndex, value);
            }
            return "";
        });

        state.parameterList.setOnContextChanged([this, instanceId](ParameterList::ParameterContext, uint8_t) {
            auto windowIt = windows_.find(instanceId);
            if (windowIt == windows_.end()) {
                return;
            }
            refreshParameters(instanceId, windowIt->second);
        });

        state.visible = true;
        refreshParameters(instanceId, state);
        refreshPresets(instanceId, state);
        windows_[instanceId] = std::move(state);
    } else {
        it->second.visible = true;
    }
}

void InstanceDetails::hideWindow(int32_t instanceId) {
    auto it = windows_.find(instanceId);
    if (it != windows_.end()) {
        it->second.visible = false;
    }
}

void InstanceDetails::removeInstance(int32_t instanceId) {
    windows_.erase(instanceId);
}

void InstanceDetails::pruneInvalidInstances(const std::vector<int32_t>& validInstanceIds) {
    std::unordered_set<int32_t> validIds(validInstanceIds.begin(), validInstanceIds.end());
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (validIds.find(it->first) == validIds.end()) {
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
}

bool InstanceDetails::isVisible(int32_t instanceId) const {
    auto it = windows_.find(instanceId);
    return it != windows_.end() && it->second.visible;
}

void InstanceDetails::refreshParametersForInstance(int32_t instanceId) {
    auto it = windows_.find(instanceId);
    if (it == windows_.end()) {
        return;
    }
    refreshParameters(instanceId, it->second);
}

void InstanceDetails::render(const RenderContext& context) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    std::vector<int32_t> detailIds;
    detailIds.reserve(windows_.size());
    for (const auto& entry : windows_) {
        detailIds.push_back(entry.first);
    }

    for (int32_t instanceId : detailIds) {
        auto it = windows_.find(instanceId);
        if (it == windows_.end()) {
            continue;
        }

        auto& detailsState = it->second;
        if (!detailsState.visible) {
            continue;
        }

        std::string windowTitle = sequencer.engine()->getPluginName(instanceId) + " (" +
                                 sequencer.getPluginFormat(instanceId) + ") - Details###Details" +
                                 std::to_string(instanceId);

        bool windowOpen = detailsState.visible;
        bool deleteRequested = false;
        std::string windowSizeId = std::format("DetailsWindow{}", instanceId);
        float baseWidth = 650.0f;
        const float viewportWidth = ImGui::GetIO().DisplaySize.x;
        if (viewportWidth > 0.0f && context.uiScale > 0.0f) {
            baseWidth = std::min(baseWidth, viewportWidth / context.uiScale);
        }
        if (context.setNextChildWindowSize) {
            context.setNextChildWindowSize(windowSizeId, ImVec2(baseWidth, 500.0f));
        }
        if (ImGui::Begin(windowTitle.c_str(), &windowOpen)) {
            if (context.updateChildWindowSizeState) {
                context.updateChildWindowSizeState(windowSizeId);
            }
            auto* instance = sequencer.engine()->getPluginInstance(instanceId);
            if (!instance) {
                ImGui::TextUnformatted("Instance is no longer available.");
            } else {
                if (sequencer.engine()->consumeParameterMetadataRefresh(instanceId)) {
                    refreshParameters(instanceId, detailsState);
                }
                if (context.buildTrackInstance) {
                    if (auto trackInstance = context.buildTrackInstance(instanceId)) {
                        bool disableShowUIButton = !trackInstance->hasUI;
                        if (disableShowUIButton) {
                            ImGui::BeginDisabled();
                        }
                        const char* uiButtonText = trackInstance->uiVisible ? "Hide UI" : "Show UI";
                        if (ImGui::Button(uiButtonText)) {
                            if (trackInstance->uiVisible) {
                                uapmd::AppModel::instance().hidePluginUI(instanceId);
                            } else {
                                uapmd::AppModel::instance().requestShowPluginUI(instanceId);
                            }
                        }
                        if (disableShowUIButton) {
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine();
                    }
                }

                if (ImGui::Button("Save State")) {
                    if (context.savePluginState) {
                        context.savePluginState(instanceId);
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Load State")) {
                    if (context.loadPluginState) {
                        context.loadPluginState(instanceId);
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Delete")) {
                    deleteRequested = true;
                }

                ImGui::Separator();

                if (detailsState.parameterList.getParameters().empty()) {
                    refreshParameters(instanceId, detailsState);
                }
                if (detailsState.presets.empty()) {
                    refreshPresets(instanceId, detailsState);
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Pitchbend:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("##Pitchbend", &detailsState.pitchBendValue,
                                       -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_NoInput)) {
                    sequencer.engine()->sendPitchBend(instanceId, detailsState.pitchBendValue);
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Chan.Pressure:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderFloat("##ChanPressure", &detailsState.channelPressureValue,
                                       0.0f, 1.0f, "%.2f", ImGuiSliderFlags_NoInput)) {
                    sequencer.engine()->sendChannelPressure(instanceId, detailsState.channelPressureValue);
                }
                detailsState.midiKeyboard.render();
                ImGui::Separator();

                ImGui::Text("Presets:");
                if (detailsState.presets.empty()) {
                    ImGui::TextDisabled("No presets available for this plugin.");
                } else {
                    std::string presetPreviewLabel = "Select preset...";
                    if (detailsState.selectedPreset >= 0 &&
                        detailsState.selectedPreset < static_cast<int>(detailsState.presets.size())) {
                        presetPreviewLabel = detailsState.presets[detailsState.selectedPreset].name;
                        if (presetPreviewLabel.empty()) {
                            presetPreviewLabel = "(Unnamed preset)";
                        }
                    }

                    if (ImGui::BeginCombo("##PresetCombo", presetPreviewLabel.c_str())) {
                        for (size_t i = 0; i < detailsState.presets.size(); i++) {
                            const bool isSelected = (detailsState.selectedPreset == static_cast<int>(i));
                            std::string displayName = detailsState.presets[i].name;
                            if (displayName.empty()) {
                                displayName = "(Unnamed preset)";
                            }
                            std::string selectableLabel = displayName + "##Preset" + std::to_string(i);
                            if (ImGui::Selectable(selectableLabel.c_str(), isSelected)) {
                                detailsState.selectedPreset = static_cast<int>(i);
                                loadSelectedPreset(instanceId, detailsState);
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                }

                ImGui::Separator();

                ImGui::Text("Parameters:");
                if (ImGui::BeginChild("ParametersChild", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
                    renderParameterControls(instanceId, detailsState);
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();

        if (!windowOpen) {
            hideWindow(instanceId);
        }

        if (deleteRequested) {
            if (context.removeInstance) {
                context.removeInstance(instanceId);
            } else {
                uapmd::AppModel::instance().removePluginInstance(instanceId);
            }
        }
    }
}

void InstanceDetails::refreshParameters(int32_t instanceId, DetailsWindowState& state) {
    auto* pal = uapmd::AppModel::instance().sequencer().engine()->getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    auto* parameterSupport = pal->parameterSupport();
    auto perNoteSelection = buildPerNoteSelection(state.parameterList);
    const bool usingPerNoteControllers = perNoteSelection.has_value();

    if (usingPerNoteControllers && !parameterSupport) {
        state.parameterList.setParameters({});
        return;
    }

    std::vector<uapmd::ParameterMetadata> parameters;
    if (usingPerNoteControllers) {
        auto pluginParams = parameterSupport->perNoteControllers(perNoteSelection->type, perNoteSelection->context);
        parameters = toParameterMetadata(pluginParams);
    } else {
        parameters = pal->parameterMetadataList();
    }

    state.parameterList.setParameters(parameters);
    if (parameters.empty()) {
        if (!usingPerNoteControllers) {
            applyParameterUpdates(instanceId, state);
        }
        return;
    }

    for (size_t i = 0; i < parameters.size(); ++i) {
        double initialValue = parameters[i].defaultPlainValue;
        if (parameterSupport) {
            double queriedValue = initialValue;
            auto status = usingPerNoteControllers
                              ? parameterSupport->getPerNoteController(perNoteSelection->context, parameters[i].index, &queriedValue)
                              : parameterSupport->getParameter(parameters[i].index, &queriedValue);
            if (status == remidy::StatusCode::OK) {
                initialValue = queriedValue;
            }
        }
        state.parameterList.setParameterValue(i, static_cast<float>(initialValue));

        auto valueString = (usingPerNoteControllers && perNoteSelection)
                               ? pal->getPerNoteControllerValueString(
                                     static_cast<uint8_t>(perNoteSelection->context.note),
                                     static_cast<uint8_t>(parameters[i].index),
                                     initialValue)
                               : pal->getParameterValueString(parameters[i].index, initialValue);
        state.parameterList.setParameterValueString(i, valueString);
    }

    if (!usingPerNoteControllers) {
        applyParameterUpdates(instanceId, state);
    }
}

void InstanceDetails::refreshPresets(int32_t instanceId, DetailsWindowState& state) {
    state.presets.clear();
    state.selectedPreset = -1;

    auto* pal = uapmd::AppModel::instance().sequencer().engine()->getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    state.presets = pal->presetMetadataList();
}

void InstanceDetails::loadSelectedPreset(int32_t instanceId, DetailsWindowState& state) {
    if (state.selectedPreset < 0 || state.selectedPreset >= static_cast<int>(state.presets.size())) {
        return;
    }

    auto* pal = uapmd::AppModel::instance().sequencer().engine()->getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    const auto& preset = state.presets[state.selectedPreset];
    pal->loadPreset(preset.index);
    std::cout << "Loading preset " << preset.name << " for instance " << instanceId << std::endl;

    refreshParameters(instanceId, state);
}

void InstanceDetails::applyParameterUpdates(int32_t instanceId, DetailsWindowState& state) {
    if (state.parameterList.context() != ParameterList::ParameterContext::Global) {
        return;
    }
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* pal = sequencer.engine()->getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    auto updates = sequencer.engine()->getParameterUpdates(instanceId);
    if (updates.empty()) {
        return;
    }

    const auto& parameters = state.parameterList.getParameters();
    bool needsRefresh = false;

    for (const auto& update : updates) {
        bool handled = false;
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (parameters[i].index == update.parameterIndex) {
                state.parameterList.setParameterValue(i, static_cast<float>(update.value));
                auto valueString = pal->getParameterValueString(parameters[i].index, update.value);
                state.parameterList.setParameterValueString(i, valueString);
                handled = true;
                break;
            }
        }
        if (!handled) {
            needsRefresh = true;
            break;
        }
    }

    if (needsRefresh) {
        refreshParameters(instanceId, state);
    }
}

void InstanceDetails::renderParameterControls(int32_t instanceId, DetailsWindowState& state) {
    applyParameterUpdates(instanceId, state);
    state.parameterList.render();
}

}
