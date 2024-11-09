#include "uapmd/uapmd.hpp"
#include <miniaudio.h>

namespace uapmd {
    class MiniAudioIODevice : public AudioIODevice {
        ma_engine_config config{};
        ma_engine engine{};
        remidy::AudioProcessContext data{0}; // no UMP events handled here.
        std::vector<std::function<uapmd_status_t(remidy::AudioProcessContext& data)>> callbacks{};

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
        uapmd_status_t start() override;
        uapmd_status_t stop() override;
    };
}