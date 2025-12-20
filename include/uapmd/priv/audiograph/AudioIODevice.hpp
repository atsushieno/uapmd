#pragma once

#include <string>
#include <vector>

namespace uapmd {
    enum AudioIODirections {
        UAPMD_AUDIO_DIRECTION_INPUT = 1,
        UAPMD_AUDIO_DIRECTION_OUTPUT = 2,
        UAPMD_AUDIO_DIRECTION_DUPLEX = 3
    };

    enum AudioIODeviceChange {
        UAPMD_AUDIO_DEVICE_CHANGE_ADDED = 1,
        UAPMD_AUDIO_DEVICE_CHANGE_REMOVED = 2
    };

    class AudioIODeviceInfo {
    public:
        AudioIODirections directions{UAPMD_AUDIO_DIRECTION_OUTPUT};
        int32_t id{};
        std::string name{};
        uint32_t sampleRate{};
        uint32_t channels{};
    };

    class AudioIODevice {
    protected:
        AudioIODevice() = default;
        virtual ~AudioIODevice() = default;

    public:
        virtual void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) = 0;
        virtual void clearAudioCallbacks() = 0;

        // FIXME: they should differ at input and output
        virtual double sampleRate() = 0;
        virtual uint32_t channels() = 0;
        virtual uint32_t inputChannels() { return channels(); }
        virtual uint32_t outputChannels() { return channels(); }

        virtual std::vector<uint32_t> getNativeSampleRates() = 0;

        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;
        virtual bool isPlaying() = 0;
    };

    class AudioIODeviceManager {
        const std::string& driver_name;

    public:
        using DeviceChangeCallback = std::function<void(int32_t deviceId, AudioIODeviceChange change)>;

        static AudioIODeviceManager* instance(const std::string& driverName = "");

        struct Configuration {
            Logger* logger{};
        };

        virtual void initialize(Configuration& config) = 0;
        std::vector<AudioIODeviceInfo> devices() {
            if (!initialized) {
                // this means even logger is not initialized, so we resort to the global logger.
                Logger::global()->logError("Attempt to use AudioIODeviceManager without initializing.");
                return {};
            }
            return onDevices();
        }

        virtual AudioIODevice* open() = 0;
        virtual std::vector<uint32_t> getDeviceSampleRates(const std::string& deviceName, AudioIODirections direction) = 0;

        void setDeviceChangeCallback(DeviceChangeCallback callback) {
            deviceChangeCallback_ = std::move(callback);
        }

    protected:
        bool initialized{false};
        DeviceChangeCallback deviceChangeCallback_;

        explicit AudioIODeviceManager(const std::string& driverName) : driver_name(driverName) {}
        virtual std::vector<AudioIODeviceInfo> onDevices() = 0;

        void notifyDeviceChange(int32_t deviceId, AudioIODeviceChange change) {
            if (deviceChangeCallback_) {
                deviceChangeCallback_(deviceId, change);
            }
        }
    };
}
