#pragma once

#include <cstdint>
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
    std::vector<int> availableBufferSizes_ = {64, 96, 128, 192, 256, 384, 512, 1024, 2048, 4096, 8192, 16384};
    int selectedInputDevice_ = 0;
    int selectedOutputDevice_ = 0;
#if defined(__ANDROID__)
    int bufferSize_ = 512;
    int selectedBufferSizeIndex_ = 6; // Default to 512
#else
    int bufferSize_ = 256;
    int selectedBufferSizeIndex_ = 4; // Default to 256
#endif
    bool platformProvidesAutoBufferSize_ = false;
    bool useAutoBufferSize_ = false;
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
    void setAvailableBufferSizes(const std::vector<int>& sizes);
    void setInputSampleRate(int rate);
    void setOutputSampleRate(int rate);

    int getSelectedInputDevice() const;
    int getSelectedOutputDevice() const;
    int getBufferSize() const;
    int getInputSampleRate() const;
    int getOutputSampleRate() const;
    void setPlatformProvidesAutoBufferSize(bool available);
    bool platformProvidesAutoBufferSize() const { return platformProvidesAutoBufferSize_; }
    void setUseAutoBufferSize(bool enabled);
    bool isAutoBufferSizeEnabled() const { return platformProvidesAutoBufferSize_ && useAutoBufferSize_; }

    void render();

    void setOnDeviceChanged(DeviceChangedCallback callback);
};

}
