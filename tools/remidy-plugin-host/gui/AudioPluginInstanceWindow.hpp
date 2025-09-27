#pragma once

#include <vector>
#include <string>
#include <uapmd/uapmd.hpp>

namespace uapmd::gui {
    class AudioPluginInstanceWindow {
        bool isOpen_ = true;
        int selectedInstance_ = -1;
        std::vector<int32_t> instances_;
        std::vector<uapmd::ParameterMetadata> parameters_;
        std::vector<uapmd::PresetsMetadata> presets_;
        int selectedPreset_ = -1;

    public:
        AudioPluginInstanceWindow();
        void render();

    private:
        void refreshInstances();
        void refreshParameters();
        void refreshPresets();
        void loadSelectedPreset();
        void renderParameterControls();
    };
}