#include "AudioDeviceSettings.hpp"

namespace uapmd::gui {

AudioDeviceSettings::AudioDeviceSettings() {
}

void AudioDeviceSettings::setInputDevices(const std::vector<std::string>& devices) {
    inputDevices_ = devices;
}

void AudioDeviceSettings::setOutputDevices(const std::vector<std::string>& devices) {
    outputDevices_ = devices;
}

void AudioDeviceSettings::setSelectedInputDevice(int index) {
    selectedInputDevice_ = index;
}

void AudioDeviceSettings::setSelectedOutputDevice(int index) {
    selectedOutputDevice_ = index;
}

void AudioDeviceSettings::setBufferSize(int size) {
    bufferSize_ = size;
}

void AudioDeviceSettings::setSampleRate(int rate) {
    sampleRate_ = rate;
}

int AudioDeviceSettings::getSelectedInputDevice() const {
    return selectedInputDevice_;
}

int AudioDeviceSettings::getSelectedOutputDevice() const {
    return selectedOutputDevice_;
}

int AudioDeviceSettings::getBufferSize() const {
    return bufferSize_;
}

int AudioDeviceSettings::getSampleRate() const {
    return sampleRate_;
}

void AudioDeviceSettings::render() {
    ImGui::Text("Audio Device Configuration:");

    // Input device selection
    if (ImGui::BeginCombo("Input Device", selectedInputDevice_ < static_cast<int>(inputDevices_.size()) ? inputDevices_[selectedInputDevice_].c_str() : "None")) {
        for (size_t i = 0; i < inputDevices_.size(); i++) {
            bool isSelected = (selectedInputDevice_ == static_cast<int>(i));
            if (ImGui::Selectable(inputDevices_[i].c_str(), isSelected)) {
                selectedInputDevice_ = static_cast<int>(i);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Output device selection
    if (ImGui::BeginCombo("Output Device", selectedOutputDevice_ < static_cast<int>(outputDevices_.size()) ? outputDevices_[selectedOutputDevice_].c_str() : "None")) {
        for (size_t i = 0; i < outputDevices_.size(); i++) {
            bool isSelected = (selectedOutputDevice_ == static_cast<int>(i));
            if (ImGui::Selectable(outputDevices_[i].c_str(), isSelected)) {
                selectedOutputDevice_ = static_cast<int>(i);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Buffer size and sample rate
    ImGui::InputInt("Buffer Size", &bufferSize_);
    ImGui::InputInt("Sample Rate", &sampleRate_);

    // Apply and refresh buttons
    if (ImGui::Button("Apply Settings")) {
        if (onApplySettings_) {
            onApplySettings_(selectedInputDevice_, selectedOutputDevice_, bufferSize_, sampleRate_);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Devices")) {
        if (onRefreshDevices_) {
            onRefreshDevices_();
        }
    }
}

void AudioDeviceSettings::setOnApplySettings(ApplySettingsCallback callback) {
    onApplySettings_ = callback;
}

void AudioDeviceSettings::setOnRefreshDevices(RefreshDevicesCallback callback) {
    onRefreshDevices_ = callback;
}

}
