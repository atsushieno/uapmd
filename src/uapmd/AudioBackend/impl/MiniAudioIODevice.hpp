#include "uapmd/uapmd.hpp"
#include <miniaudio.h>

namespace uapmd {
    class MiniAudioIODeviceManager : public AudioIODeviceManager {
        ma_context context{};
        ma_log ma_logger{};
        Logger* remidy_logger{};

        static void on_ma_log(void* userData, uint32_t logLevel, const char* message);
    public:
        MiniAudioIODeviceManager();
        void initialize(Configuration& config) override;
        AudioIODevice * open() override;

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
        uapmd_status_t start() override;
        uapmd_status_t stop() override;
        bool isPlaying() override;
    };
}
