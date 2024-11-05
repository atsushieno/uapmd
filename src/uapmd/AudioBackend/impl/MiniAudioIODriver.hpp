#include "uapmd/uapmd.hpp"
#include <miniaudio.h>

namespace uapmd {
    class MiniAudioIODriver : public AudioIODriver {
        ma_engine_config config{};
        ma_engine engine{};
        AudioProcessContext data{0}; // no UMP events handled here.
        std::vector<std::function<uapmd_status_t(AudioProcessContext& data)>> callbacks{};

    public:
        MiniAudioIODriver();
        ~MiniAudioIODriver() override;

        void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) override {
            callbacks.emplace_back(std::move(callback));
        }
        void dataCallback(void* output, const void* input, ma_uint32 frameCount);
        uapmd_status_t start() override;
        uapmd_status_t stop() override;
    };
}
