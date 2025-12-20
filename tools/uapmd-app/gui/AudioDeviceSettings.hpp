#pragma once

#include <vector>
#include <string>
#include <functional>
#include <imgui.h>

namespace uapmd::gui {

class AudioDeviceSettings {
public:
    using ApplySettingsCallback = std::function<void(int inputDeviceIndex, int outputDeviceIndex, int bufferSize, int sampleRate)>;
    using RefreshDevicesCallback = std::function<void()>;

private:
    std::vector<std::string> inputDevices_;
    std::vector<std::string> outputDevices_;
    int selectedInputDevice_ = 0;
    int selectedOutputDevice_ = 0;
    int bufferSize_ = 512;
    int sampleRate_ = 44100;

    ApplySettingsCallback onApplySettings_;
    RefreshDevicesCallback onRefreshDevices_;

public:
    AudioDeviceSettings();

    void setInputDevices(const std::vector<std::string>& devices);
    void setOutputDevices(const std::vector<std::string>& devices);
    void setSelectedInputDevice(int index);
    void setSelectedOutputDevice(int index);
    void setBufferSize(int size);
    void setSampleRate(int rate);

    int getSelectedInputDevice() const;
    int getSelectedOutputDevice() const;
    int getBufferSize() const;
    int getSampleRate() const;

    void render();

    void setOnApplySettings(ApplySettingsCallback callback);
    void setOnRefreshDevices(RefreshDevicesCallback callback);
};

}
