#define MINIAUDIO_IMPLEMENTATION 1
#include "MiniAudioIODevice.hpp"
#include <choc/audio/choc_SampleBuffers.h>

static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    ((uapmd::MiniAudioIODevice*) device->pUserData)->dataCallback(output, input, frameCount);
}

uapmd::MiniAudioIODevice::MiniAudioIODevice(const std::string& deviceName) {
    config = ma_engine_config_init();
    config.dataCallback = data_callback;
    config.periodSizeInFrames = 1024; // FIXME: provide audio buffer size
    // FIXME: support explicit device specification by `deviceName`,

    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        throw std::runtime_error("uapmd: Failed to initialize miniaudio driver.");
    }
    engine.pDevice->pUserData = this;
}

uapmd::MiniAudioIODevice::~MiniAudioIODevice() {
    ma_engine_uninit(&engine);
}

uapmd_status_t uapmd::MiniAudioIODevice::start() {
    auto device = ma_engine_get_device(&engine);
    if (device->capture.channels)
        data.addAudioIn(device->capture.channels, config.periodSizeInFrames);
    if (device->playback.channels)
        data.addAudioOut(device->playback.channels, config.periodSizeInFrames);
    ma_engine_start(&engine);

    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

uapmd_status_t uapmd::MiniAudioIODevice::stop() {
    ma_engine_stop(&engine);
    data = AudioProcessContext{0, data.trackContext()};

    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

void uapmd::MiniAudioIODevice::dataCallback(void *output, const void *input, ma_uint32 frameCount) {
    size_t framesPerCallback = 4096;
    if (data.audioInBusCount() > 0) {
        // FIXME: get appropriate main bus
        auto mainBusIn = data.audioIn(0);
        // FIXME: it should be pre-allocated elsewhere
        auto inputView = choc::buffer::createChannelArrayView((float* const *) input, mainBusIn->channelCount(), frameCount);
        inputView.data.channels = (float* const *) input;
        for (size_t i = 0, n = mainBusIn->channelCount(); i < n; i++)
            memcpy(mainBusIn->getFloatBufferForChannel(i), inputView.getChannel(i).data.data, sizeof(float) * frameCount);
    }
    data.frameCount(frameCount);

    for (auto& callback : callbacks)
        callback(data);

    if (data.audioOutBusCount() > 0) {
        // FIXME: get appropriate main bus
        auto mainBusOut = data.audioOut(0);
        // FIXME: it should be pre-allocated elsewhere
        auto outputView = choc::buffer::createInterleavedView((float*) input, mainBusOut->channelCount(), frameCount);
        outputView.data.data = (float*) output;
        for (size_t i = 0, n = mainBusOut->channelCount(); i < n; i++)
            memcpy(static_cast<void *>(outputView.getChannel(i).data.data), mainBusOut->getFloatBufferForChannel(i), sizeof(float) * frameCount);

    }
}
