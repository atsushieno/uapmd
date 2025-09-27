#include "AudioPluginInstanceWindow.hpp"
#include "../AppModel.hpp"
#include <imgui.h>
#include <iostream>

namespace uapmd::gui {

AudioPluginInstanceWindow::AudioPluginInstanceWindow() {
    refreshInstances();
}

void AudioPluginInstanceWindow::render() {
    if (!ImGui::Begin("Plugin Instance Control", &isOpen_)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Refresh Instances")) {
        refreshInstances();
    }

    ImGui::Separator();

    // Instance selection
    ImGui::Text("Active Instances:");
    if (ImGui::BeginListBox("##InstanceList", ImVec2(-1, 100))) {
        for (size_t i = 0; i < instances_.size(); i++) {
            const bool isSelected = (selectedInstance_ == static_cast<int>(i));
            std::string label = "Instance " + std::to_string(instances_[i]);
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                selectedInstance_ = static_cast<int>(i);
                refreshParameters();
                refreshPresets();
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndListBox();
    }

    if (selectedInstance_ >= 0 && selectedInstance_ < static_cast<int>(instances_.size())) {
        int32_t instanceId = instances_[selectedInstance_];

        ImGui::Separator();

        // Preset management
        ImGui::Text("Presets:");
        if (ImGui::BeginCombo("##PresetCombo", selectedPreset_ >= 0 ? presets_[selectedPreset_].name.c_str() : "Select preset...")) {
            for (size_t i = 0; i < presets_.size(); i++) {
                const bool isSelected = (selectedPreset_ == static_cast<int>(i));
                if (ImGui::Selectable(presets_[i].name.c_str(), isSelected)) {
                    selectedPreset_ = static_cast<int>(i);
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        bool canLoadPreset = selectedPreset_ >= 0 && selectedPreset_ < static_cast<int>(presets_.size());
        if (!canLoadPreset) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Load Preset")) {
            loadSelectedPreset();
        }
        if (!canLoadPreset) {
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        // Parameters
        ImGui::Text("Parameters:");
        renderParameterControls();
    }

    ImGui::End();
}

void AudioPluginInstanceWindow::refreshInstances() {
    instances_.clear();

    // TODO: Get actual instance list from sequencer
    // For now, create placeholder instances
    instances_.push_back(1);
    instances_.push_back(2);

    // Reset selection if out of bounds
    if (selectedInstance_ >= static_cast<int>(instances_.size())) {
        selectedInstance_ = -1;
        parameters_.clear();
        presets_.clear();
    }
}

void AudioPluginInstanceWindow::refreshParameters() {
    parameters_.clear();

    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];

    // TODO: Get parameters from the actual plugin instance
    // This would call into the WebView component's getParameterList function
    // parameters_ = getParameterList(instanceId);

    // Get parameters from the real sequencer (same as WebView version)
    parameters_ = uapmd::AppModel::instance().sequencer().getParameterList(instanceId);
}

void AudioPluginInstanceWindow::refreshPresets() {
    presets_.clear();

    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];

    // TODO: Get presets from the actual plugin instance
    // This would call into the WebView component's getPresetList function
    // presets_ = getPresetList(instanceId);

    // Get presets from the real sequencer (same as WebView version)
    presets_ = uapmd::AppModel::instance().sequencer().getPresetList(instanceId);

    selectedPreset_ = -1;
}

void AudioPluginInstanceWindow::loadSelectedPreset() {
    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size()) ||
        selectedPreset_ < 0 || selectedPreset_ >= static_cast<int>(presets_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];
    int32_t presetIndex = presets_[selectedPreset_].index;

    // Load preset using the real sequencer (same as WebView version)
    uapmd::AppModel::instance().sequencer().loadPreset(instanceId, presetIndex);

    std::cout << "Loading preset " << presets_[selectedPreset_].name
              << " for instance " << instanceId << std::endl;

    // Refresh parameters after preset load
    refreshParameters();
}

void AudioPluginInstanceWindow::renderParameterControls() {
    if (ImGui::BeginTable("ParameterTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (auto& param : parameters_) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%s", param.name.c_str());

            ImGui::TableNextColumn();
            std::string sliderId = "##" + std::to_string(param.index);
            float paramValue = static_cast<float>(param.initialValue); // Use current value if available
            if (ImGui::SliderFloat(sliderId.c_str(), &paramValue, static_cast<float>(param.minValue), static_cast<float>(param.maxValue))) {
                // Send parameter change to the real sequencer (same as WebView version)
                int32_t instanceId = instances_[selectedInstance_];
                uapmd::AppModel::instance().sequencer().setParameterValue(instanceId, param.index, paramValue);
                std::cout << "Parameter " << param.name << " changed to " << paramValue << std::endl;
            }

            ImGui::TableNextColumn();
            std::string resetId = "Reset##" + std::to_string(param.index);
            if (ImGui::Button(resetId.c_str())) {
                paramValue = static_cast<float>(param.initialValue);
                // TODO: Send parameter reset to the actual plugin instance
            }
        }

        ImGui::EndTable();
    }
}

}