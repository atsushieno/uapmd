#include "uapmd/uapmd.hpp"

namespace uapmd {
    class MiniAudioIODriver : public AudioIODriver {
    public:
        void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) override;
        uapmd_status_t start() override;
        uapmd_status_t stop() override;
    };
}
