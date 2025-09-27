#pragma once

#include <vector>
#include <string>

namespace uapmd::gui {
    class AudioDeviceSetupWindow {
        bool isOpen_ = true;
        int selectedInputDevice_ = -1;
        int selectedOutputDevice_ = -1;
        int bufferSize_ = 512;
        int sampleRate_ = 44100;

        std::vector<std::string> inputDevices_;
        std::vector<std::string> outputDevices_;

    public:
        AudioDeviceSetupWindow();
        void render();

    private:
        void refreshDevices();
        void applySettings();
    };
}