#pragma once

#include <string>
#include <vector>

namespace uapmd::gui {
    class AudioPlayerWindow {
        bool isOpen_ = true;
        bool isPlaying_ = false;
        bool isRecording_ = false;
        float playbackPosition_ = 0.0f;
        float playbackLength_ = 100.0f; // in seconds
        float volume_ = 0.75f;

        std::string currentFile_;
        std::vector<std::string> recentFiles_;

    public:
        AudioPlayerWindow();
        void render();

    private:
        void playPause();
        void stop();
        void record();
        void loadFile();
        void renderTransportControls();
        void renderVolumeControl();
        void renderFileSelection();
    };
}