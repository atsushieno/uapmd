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

void AudioDeviceSettings::setInputAvailableSampleRates(const std::vector<uint32_t>& sampleRates) {
    inputAvailableSampleRates_ = sampleRates;
    // Try to find current sample rate in the list and set the index
    selectedInputSampleRateIndex_ = 0;
    for (size_t i = 0; i < inputAvailableSampleRates_.size(); i++) {
        if (static_cast<int>(inputAvailableSampleRates_[i]) == inputSampleRate_) {
            selectedInputSampleRateIndex_ = static_cast<int>(i);
            break;
        }
    }
}

void AudioDeviceSettings::setOutputAvailableSampleRates(const std::vector<uint32_t>& sampleRates) {
    outputAvailableSampleRates_ = sampleRates;
    // Try to find current sample rate in the list and set the index
    selectedOutputSampleRateIndex_ = 0;
    for (size_t i = 0; i < outputAvailableSampleRates_.size(); i++) {
        if (static_cast<int>(outputAvailableSampleRates_[i]) == outputSampleRate_) {
            selectedOutputSampleRateIndex_ = static_cast<int>(i);
            break;
        }
    }
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

void AudioDeviceSettings::setInputSampleRate(int rate) {
    inputSampleRate_ = rate;
}

void AudioDeviceSettings::setOutputSampleRate(int rate) {
    outputSampleRate_ = rate;
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

int AudioDeviceSettings::getInputSampleRate() const {
    return inputSampleRate_;
}

int AudioDeviceSettings::getOutputSampleRate() const {
    return outputSampleRate_;
}

void AudioDeviceSettings::render() {
    ImGui::Text("Audio Device Configuration:");

    // Input device selection
    if (ImGui::BeginCombo("Input Device", selectedInputDevice_ < static_cast<int>(inputDevices_.size()) ? inputDevices_[selectedInputDevice_].c_str() : "None")) {
        for (size_t i = 0; i < inputDevices_.size(); i++) {
            bool isSelected = (selectedInputDevice_ == static_cast<int>(i));
            if (ImGui::Selectable(inputDevices_[i].c_str(), isSelected)) {
                selectedInputDevice_ = static_cast<int>(i);
                if (onDeviceChanged_) {
                    onDeviceChanged_();
                }
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
                if (onDeviceChanged_) {
                    onDeviceChanged_();
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Buffer size
    ImGui::InputInt("Buffer Size", &bufferSize_);

    // Input sample rate dropdown
    std::string inputSampleRateLabel = selectedInputSampleRateIndex_ < static_cast<int>(inputAvailableSampleRates_.size())
        ? std::to_string(inputAvailableSampleRates_[selectedInputSampleRateIndex_]) + " Hz"
        : "No sample rates available";

    if (ImGui::BeginCombo("Input Sample Rate", inputSampleRateLabel.c_str())) {
        for (size_t i = 0; i < inputAvailableSampleRates_.size(); i++) {
            bool isSelected = (selectedInputSampleRateIndex_ == static_cast<int>(i));
            std::string label = std::to_string(inputAvailableSampleRates_[i]) + " Hz";
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                selectedInputSampleRateIndex_ = static_cast<int>(i);
                inputSampleRate_ = static_cast<int>(inputAvailableSampleRates_[i]);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Output sample rate dropdown
    std::string outputSampleRateLabel = selectedOutputSampleRateIndex_ < static_cast<int>(outputAvailableSampleRates_.size())
        ? std::to_string(outputAvailableSampleRates_[selectedOutputSampleRateIndex_]) + " Hz"
        : "No sample rates available";

    if (ImGui::BeginCombo("Output Sample Rate", outputSampleRateLabel.c_str())) {
        for (size_t i = 0; i < outputAvailableSampleRates_.size(); i++) {
            bool isSelected = (selectedOutputSampleRateIndex_ == static_cast<int>(i));
            std::string label = std::to_string(outputAvailableSampleRates_[i]) + " Hz";
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                selectedOutputSampleRateIndex_ = static_cast<int>(i);
                outputSampleRate_ = static_cast<int>(outputAvailableSampleRates_[i]);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

}

void AudioDeviceSettings::setOnDeviceChanged(DeviceChangedCallback callback) {
    onDeviceChanged_ = callback;
}

}
