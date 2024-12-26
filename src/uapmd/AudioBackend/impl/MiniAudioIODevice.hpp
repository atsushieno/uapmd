#include "uapmd/uapmd.hpp"
#include <miniaudio.h>

namespace uapmd {
    class MiniAudioIODeviceManager : public AudioIODeviceManager {
        std::vector<ma_backend> backends{};
        ma_context context{};
        ma_log ma_logger{};
        remidy::Logger* remidy_logger{};

        static void on_ma_log(void* userData, uint32_t logLevel, const char* message);
    public:
        MiniAudioIODeviceManager();
        void initialize(Configuration& config) override;
        AudioIODevice * activeDefaultDevice() override;

    protected:
        std::vector<AudioIODeviceInfo> onDevices() override;
    };

    class MiniAudioIODevice : public AudioIODevice {
        ma_engine_config config{};
        ma_engine engine{};
        remidy::MasterContext master_context{};
        remidy::AudioProcessContext data; // no UMP events handled here.
        std::vector<std::function<uapmd_status_t(remidy::AudioProcessContext& data)>> callbacks{};
        std::vector<const float *> dataOutPtrs{};

    public:
        explicit MiniAudioIODevice(const std::string& deviceName);
        ~MiniAudioIODevice() override;

        void addAudioCallback(std::function<uapmd_status_t(remidy::AudioProcessContext& data)>&& callback) override {
            callbacks.emplace_back(std::move(callback));
        }
        void clearAudioCallbacks() override {
            callbacks.clear();
        }
        void dataCallback(void* output, const void* input, ma_uint32 frameCount);
        double sampleRate() override;
        uint32_t channels() override;
        uapmd_status_t start() override;
        uapmd_status_t stop() override;
        bool isPlaying() override;
    };
}
