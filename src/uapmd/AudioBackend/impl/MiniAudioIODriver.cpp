#define MINIAUDIO_IMPLEMENTATION 1
#include "MiniAudioIODriver.hpp"

static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    ((uapmd::MiniAudioIODriver*) device->pUserData)->dataCallback(output, input, frameCount);
}

uapmd::MiniAudioIODriver::MiniAudioIODriver() {
    config = ma_engine_config_init();
    config.dataCallback = data_callback;
    config.pContext->pUserData = this;

    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        throw std::runtime_error("uapmd: Failed to initialize miniaudio driver.");
    }
}

uapmd::MiniAudioIODriver::~MiniAudioIODriver() {
    ma_engine_uninit(&engine);
}

uapmd_status_t uapmd::MiniAudioIODriver::start() {
    ma_engine_start(&engine);
    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

uapmd_status_t uapmd::MiniAudioIODriver::stop() {
    ma_engine_stop(&engine);
    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

void uapmd::MiniAudioIODriver::dataCallback(void *output, const void *input, ma_uint32 frameCount) {

}
