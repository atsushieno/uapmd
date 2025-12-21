#pragma once

#include <vector>
#include <string>
#include <functional>
#include <imgui.h>

namespace uapmd::gui {

class AudioDeviceSettings {
public:
    using DeviceChangedCallback = std::function<void()>;

private:
    std::vector<std::string> inputDevices_;
    std::vector<std::string> outputDevices_;
    std::vector<uint32_t> inputAvailableSampleRates_;
    std::vector<uint32_t> outputAvailableSampleRates_;
    int selectedInputDevice_ = 0;
    int selectedOutputDevice_ = 0;
    int bufferSize_ = 512;
    int inputSampleRate_ = 48000;
    int outputSampleRate_ = 48000;
    int selectedInputSampleRateIndex_ = 0;
    int selectedOutputSampleRateIndex_ = 0;

    DeviceChangedCallback onDeviceChanged_;

public:
    AudioDeviceSettings();

    void setInputDevices(const std::vector<std::string>& devices);
    void setOutputDevices(const std::vector<std::string>& devices);
    void setInputAvailableSampleRates(const std::vector<uint32_t>& sampleRates);
    void setOutputAvailableSampleRates(const std::vector<uint32_t>& sampleRates);
    void setSelectedInputDevice(int index);
    void setSelectedOutputDevice(int index);
    void setBufferSize(int size);
    void setInputSampleRate(int rate);
    void setOutputSampleRate(int rate);

    int getSelectedInputDevice() const;
    int getSelectedOutputDevice() const;
    int getBufferSize() const;
    int getInputSampleRate() const;
    int getOutputSampleRate() const;

    void render();

    void setOnDeviceChanged(DeviceChangedCallback callback);
};

}
