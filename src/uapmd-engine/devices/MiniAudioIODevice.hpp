#pragma once

#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"
#include <miniaudio.h>

namespace uapmd {
    class MiniAudioIODeviceManager : public AudioIODeviceManager {
        ma_context context{};
        ma_log ma_logger{};
        Logger* remidy_logger{};

        static void on_ma_log(void* userData, uint32_t logLevel, const char* message);

    protected:
        AudioIODevice* onOpen(int inputDeviceIndex, int outputDeviceIndex, uint32_t sampleRate) override;

    public:
        static void on_ma_device_notification(const ma_device_notification* pNotification);
        MiniAudioIODeviceManager();
        ~MiniAudioIODeviceManager() override;
        void initialize(Configuration& config) override;
        std::vector<uint32_t> getDeviceSampleRates(const std::string& deviceName, AudioIODirections direction) override;

        ma_context& maContext() { return context; }

    protected:
        std::vector<AudioIODeviceInfo> onDevices() override;
    };

    class MiniAudioIODevice : public AudioIODevice {
        ma_engine_config config{};
        ma_engine engine{};
        MasterContext master_context{};
        AudioProcessContext data; // no UMP events handled here.
        std::vector<std::function<uapmd_status_t(AudioProcessContext& data)>> callbacks{};
        std::vector<const float *> dataOutPtrs{};
        uint32_t input_channels{0};
        uint32_t output_channels{0};
        MiniAudioIODeviceManager* manager_{nullptr};

    public:
        explicit MiniAudioIODevice(MiniAudioIODeviceManager* manager);
        ~MiniAudioIODevice() override;

        void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) override {
            callbacks.emplace_back(std::move(callback));
        }
        void clearAudioCallbacks() override {
            callbacks.clear();
        }
        void dataCallback(void* output, const void* input, ma_uint32 frameCount);
        double sampleRate() override;
        uint32_t channels() override;
        uint32_t inputChannels() override;
        uint32_t outputChannels() override;
        std::vector<uint32_t> getNativeSampleRates() override;
        uapmd_status_t start() override;
        uapmd_status_t stop() override;
        bool isPlaying() override;

        // Reconfigure the device with new device IDs and sample rate
        bool reconfigure(const ma_device_id* inputDeviceId, const ma_device_id* outputDeviceId, uint32_t sampleRate = 0);

        [[nodiscard]] MiniAudioIODeviceManager* getManager() const { return manager_; }

    private:
        bool initializeDuplexDevice(const ma_device_id* inputDeviceId, const ma_device_id* outputDeviceId, uint32_t sampleRate);
    };
}
