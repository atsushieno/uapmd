#include "AudioPlayerWindow.hpp"
#include "../AppModel.hpp"
#include <imgui.h>
#include <iostream>

namespace uapmd::gui {

AudioPlayerWindow::AudioPlayerWindow() {
    // Initialize with some example recent files
    recentFiles_.push_back("example1.wav");
    recentFiles_.push_back("example2.mid");
    recentFiles_.push_back("example3.wav");
}

void AudioPlayerWindow::render() {
    if (!ImGui::Begin("Audio Player", &isOpen_)) {
        ImGui::End();
        return;
    }

    renderFileSelection();
    ImGui::Separator();
    renderTransportControls();
    ImGui::Separator();
    renderVolumeControl();

    ImGui::End();
}

void AudioPlayerWindow::renderFileSelection() {
    ImGui::Text("Current File: %s", currentFile_.empty() ? "None" : currentFile_.c_str());

    if (ImGui::Button("Load File...")) {
        loadFile();
    }

    if (!recentFiles_.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("Recent Files", "Recent...")) {
            for (const auto& file : recentFiles_) {
                if (ImGui::Selectable(file.c_str())) {
                    currentFile_ = file;
                    // TODO: Actually load the file
                    std::cout << "Loading file: " << currentFile_ << std::endl;
                }
            }
            ImGui::EndCombo();
        }
    }
}

void AudioPlayerWindow::renderTransportControls() {
    ImGui::Text("Transport Controls:");

    // Play/Pause button
    const char* playButtonText = isPlaying_ ? "Pause" : "Play";
    if (ImGui::Button(playButtonText)) {
        playPause();
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        stop();
    }

    ImGui::SameLine();
    const char* recordButtonText = isRecording_ ? "Stop Recording" : "Record";
    if (ImGui::Button(recordButtonText)) {
        record();
    }

    // Position slider
    ImGui::Text("Position:");
    if (ImGui::SliderFloat("##Position", &playbackPosition_, 0.0f, playbackLength_, "%.1f s")) {
        // TODO: Seek to position
        std::cout << "Seeking to position: " << playbackPosition_ << std::endl;
    }

    // Time display
    int currentMin = static_cast<int>(playbackPosition_) / 60;
    int currentSec = static_cast<int>(playbackPosition_) % 60;
    int totalMin = static_cast<int>(playbackLength_) / 60;
    int totalSecTotal = static_cast<int>(playbackLength_) % 60;

    ImGui::Text("Time: %02d:%02d / %02d:%02d", currentMin, currentSec, totalMin, totalSecTotal);
}

void AudioPlayerWindow::renderVolumeControl() {
    ImGui::Text("Master Volume:");
    if (ImGui::SliderFloat("##Volume", &volume_, 0.0f, 1.0f, "%.2f")) {
        // TODO: Apply volume change to audio system
        std::cout << "Volume changed to: " << volume_ << std::endl;
    }

    // Mute button
    static bool isMuted = false;
    if (ImGui::Checkbox("Mute", &isMuted)) {
        // TODO: Apply mute state to audio system
        std::cout << "Mute state: " << (isMuted ? "ON" : "OFF") << std::endl;
    }
}

void AudioPlayerWindow::playPause() {
    // Use real audio control (same as WebView version)
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    if (isPlaying_) {
        sequencer.stopAudio();
        isPlaying_ = false;
        std::cout << "Stopping playback" << std::endl;
    } else {
        sequencer.startAudio();
        isPlaying_ = true;
        std::cout << "Starting playback" << std::endl;
    }
}

void AudioPlayerWindow::stop() {
    // Use real audio control (same as WebView version)
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.stopAudio();

    isPlaying_ = false;
    playbackPosition_ = 0.0f;

    std::cout << "Stopping playback" << std::endl;
}

void AudioPlayerWindow::record() {
    isRecording_ = !isRecording_;

    if (isRecording_) {
        // TODO: Start recording
        std::cout << "Starting recording" << std::endl;
    } else {
        // TODO: Stop recording
        std::cout << "Stopping recording" << std::endl;
    }
}

void AudioPlayerWindow::loadFile() {
    // TODO: Open file dialog and load selected file
    // For now, just simulate loading a file
    currentFile_ = "loaded_file.wav";
    playbackLength_ = 120.0f; // 2 minutes
    playbackPosition_ = 0.0f;

    // Add to recent files if not already there
    auto it = std::find(recentFiles_.begin(), recentFiles_.end(), currentFile_);
    if (it == recentFiles_.end()) {
        recentFiles_.insert(recentFiles_.begin(), currentFile_);
        if (recentFiles_.size() > 10) { // Keep only 10 recent files
            recentFiles_.pop_back();
        }
    }

    std::cout << "File loaded: " << currentFile_ << std::endl;
}

}