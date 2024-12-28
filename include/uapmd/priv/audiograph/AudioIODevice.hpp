#pragma once

#include <string>

namespace uapmd {
    enum AudioIODirections {
        Input = 1,
        Output = 2,
        Duplex = 3
    };

    class AudioIODeviceInfo {
    public:
        AudioIODirections directions{AudioIODirections::Output};
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

        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;
        virtual bool isPlaying() = 0;
    };

    class AudioIODeviceManager {
        const std::string& driver_name;

    public:
        static AudioIODeviceManager* instance(const std::string& driverName = "");

        struct Configuration {
            remidy::Logger* logger{};
        };

        virtual void initialize(Configuration& config) = 0;
        std::vector<AudioIODeviceInfo> devices() {
            if (!initialized) {
                // this means even logger is not initialized, so we resort to the global logger.
                remidy::Logger::global()->logError("Attempt to use AudioIODeviceManager without initializing.");
                return {};
            }
            return onDevices();
        }

        virtual AudioIODevice* open() = 0;

    protected:
        bool initialized{false};
        explicit AudioIODeviceManager(const std::string& driverName) : driver_name(driverName) {}
        virtual std::vector<AudioIODeviceInfo> onDevices() = 0;
    };
}