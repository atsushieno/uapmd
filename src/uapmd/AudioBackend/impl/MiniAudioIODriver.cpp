#define MINIAUDIO_IMPLEMENTATION 1
#include "MiniAudioIODriver.hpp"
#include <choc/audio/choc_SampleBuffers.h>

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
    data.addAudioIn(config.pDevice->capture.channels, config.periodSizeInFrames);
    data.addAudioOut(config.pDevice->playback.channels, config.periodSizeInFrames);
    ma_engine_start(&engine);

    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

uapmd_status_t uapmd::MiniAudioIODriver::stop() {
    ma_engine_stop(&engine);
    data = AudioProcessContext{0};

    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

void uapmd::MiniAudioIODriver::dataCallback(void *output, const void *input, ma_uint32 frameCount) {
    choc::buffer::BufferView<float, choc::buffer::SeparateChannelLayout> view{};
    if (data.audioInBusCount() > 0) {
        // FIXME: get appropriate main bus
        auto mainBusIn = data.audioIn(0);
        for (size_t i = 0, n = mainBusIn->channelCount(); i < n; i++)
            memcpy(mainBusIn->getFloatBufferForChannel(i), view.getChannel(i).data.data, sizeof(float) * frameCount);
        data.frameCount(frameCount);
    }
    for (auto& callback : callbacks)
        callback(data);

    if (data.audioOutBusCount() > 0) {
        // FIXME: get appropriate main bus
        auto mainBusOut = data.audioOut(0);
        for (size_t i = 0, n = mainBusOut->channelCount(); i < n; i++)
            memcpy(view.getChannel(i).data.data, mainBusOut->getFloatBufferForChannel(i), sizeof(float) * frameCount);

    }
}
