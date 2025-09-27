#include "AudioDeviceSetupWindow.hpp"
#include "../AppModel.hpp"
#include <imgui.h>
#include <iostream>
#include <uapmd/uapmd.hpp>

namespace uapmd::gui {

AudioDeviceSetupWindow::AudioDeviceSetupWindow() {
    refreshDevices();
}

void AudioDeviceSetupWindow::render() {
    if (!ImGui::Begin("Audio Device Setup", &isOpen_)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Refresh Devices")) {
        refreshDevices();
    }

    ImGui::Separator();

    // Input device selection
    ImGui::Text("Input Device:");
    if (ImGui::BeginCombo("##InputDevice", selectedInputDevice_ >= 0 ? inputDevices_[selectedInputDevice_].c_str() : "Select...")) {
        for (size_t i = 0; i < inputDevices_.size(); i++) {
            const bool isSelected = (selectedInputDevice_ == static_cast<int>(i));
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
    ImGui::Text("Output Device:");
    if (ImGui::BeginCombo("##OutputDevice", selectedOutputDevice_ >= 0 ? outputDevices_[selectedOutputDevice_].c_str() : "Select...")) {
        for (size_t i = 0; i < outputDevices_.size(); i++) {
            const bool isSelected = (selectedOutputDevice_ == static_cast<int>(i));
            if (ImGui::Selectable(outputDevices_[i].c_str(), isSelected)) {
                selectedOutputDevice_ = static_cast<int>(i);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // Buffer size
    ImGui::Text("Buffer Size:");
    const char* bufferSizes[] = { "64", "128", "256", "512", "1024", "2048" };
    int bufferSizeIndex = 3; // Default to 512
    for (int i = 0; i < 6; i++) {
        if (std::stoi(bufferSizes[i]) == bufferSize_) {
            bufferSizeIndex = i;
            break;
        }
    }
    if (ImGui::Combo("##BufferSize", &bufferSizeIndex, bufferSizes, 6)) {
        bufferSize_ = std::stoi(bufferSizes[bufferSizeIndex]);
    }

    // Sample rate
    ImGui::Text("Sample Rate:");
    const char* sampleRates[] = { "44100", "48000", "88200", "96000" };
    int sampleRateIndex = 0; // Default to 44100
    for (int i = 0; i < 4; i++) {
        if (std::stoi(sampleRates[i]) == sampleRate_) {
            sampleRateIndex = i;
            break;
        }
    }
    if (ImGui::Combo("##SampleRate", &sampleRateIndex, sampleRates, 4)) {
        sampleRate_ = std::stoi(sampleRates[sampleRateIndex]);
    }

    ImGui::Separator();

    if (ImGui::Button("Apply Settings")) {
        applySettings();
    }

    ImGui::End();
}

void AudioDeviceSetupWindow::refreshDevices() {
    // Use real device enumeration from uapmd (same as WebView version)
    inputDevices_.clear();
    outputDevices_.clear();

    auto manager = uapmd::AudioIODeviceManager::instance();
    auto devices = manager->devices();

    for (auto& d : devices) {
        if (d.directions & AudioIODirections::Input) {
            inputDevices_.push_back(d.name);
        }
        if (d.directions & AudioIODirections::Output) {
            outputDevices_.push_back(d.name);
        }
    }

    if (selectedInputDevice_ < 0 && !inputDevices_.empty()) {
        selectedInputDevice_ = 0;
    }
    if (selectedOutputDevice_ < 0 && !outputDevices_.empty()) {
        selectedOutputDevice_ = 0;
    }
}

void AudioDeviceSetupWindow::applySettings() {
    // TODO: Apply settings to the actual audio system
    // This would typically involve:
    // - Stopping current audio
    // - Reconfiguring the audio system with new settings
    // - Restarting audio

    // For now, just log the settings
    std::cout << "Applied audio settings - Buffer: " << bufferSize_
              << ", Sample Rate: " << sampleRate_ << std::endl;
}

}