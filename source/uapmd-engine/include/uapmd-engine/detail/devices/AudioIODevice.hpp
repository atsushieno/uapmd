#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uapmd {

    // Invoked once on each worker before it accepts audio jobs. The returned
    // token is destroyed on that same thread after its last job (e.g. to leave
    // a platform audio workgroup). Captured device resources must be owned.
    using AudioWorkerThreadSetup = std::function<std::shared_ptr<void>()>;

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

        virtual void setPreferredCallbackSize(uint32_t framesPerCallback) = 0;
        virtual uint32_t preferredCallbackSize() const = 0;

        // Control thread only, while device callbacks are stopped.
        virtual AudioWorkerThreadSetup audioWorkerThreadSetup() = 0;

        virtual double sampleRate() = 0;
        virtual uint32_t inputChannels() = 0;
        virtual uint32_t outputChannels() = 0;

        virtual std::vector<uint32_t> getNativeSampleRates() = 0;

        virtual void clearOutputBuffers() = 0;
        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;
        virtual bool isPlaying() = 0;
        virtual bool useAutoBufferSize() = 0;
        virtual bool useAutoBufferSize(bool value) = 0;
    };

    class AudioIODeviceManager {
        const std::string& driver_name;

    public:
        virtual ~AudioIODeviceManager() = default;

        // Passed as inputDeviceIndex/outputDeviceIndex to request that direction be
        // left closed entirely, rather than falling back to the system default (-1).
        static constexpr int kNoDeviceIndex = -2;

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

        AudioIODevice* open(int inputDeviceIndex = -1,
                            int outputDeviceIndex = -1,
                            uint32_t sampleRate = 0,
                            uint32_t bufferSize = 0) {
            return onOpen(inputDeviceIndex, outputDeviceIndex, sampleRate, bufferSize);
        }
        // An empty deviceName selects the system default for the requested direction.
        virtual std::vector<uint32_t> getDeviceSampleRates(const std::string& deviceName, AudioIODirections direction) = 0;

        void setDeviceChangeCallback(DeviceChangeCallback callback) {
            deviceChangeCallback_ = std::move(callback);
        }

        virtual bool platformProvidesAutoBufferSize() const = 0;

        // Returns the set of valid buffer sizes for this driver, or empty if the
        // UI should use its own default list.
        virtual std::vector<int> getAvailableBufferSizes() const { return {}; }

    protected:
        bool initialized{false};
        DeviceChangeCallback deviceChangeCallback_;

        explicit AudioIODeviceManager(const std::string& driverName) : driver_name(driverName) {}
        virtual std::vector<AudioIODeviceInfo> onDevices() = 0;
        virtual AudioIODevice* onOpen(int inputDeviceIndex,
                                      int outputDeviceIndex,
                                      uint32_t sampleRate,
                                      uint32_t bufferSize) = 0;

        void notifyDeviceChange(int32_t deviceId, AudioIODeviceChange change) const {
            if (deviceChangeCallback_) {
                deviceChangeCallback_(deviceId, change);
            }
        }
    };
}
