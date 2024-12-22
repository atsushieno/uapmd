#define MINIAUDIO_IMPLEMENTATION 1
#include "MiniAudioIODevice.hpp"
#include <choc/audio/choc_SampleBuffers.h>

static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    ((uapmd::MiniAudioIODevice*) device->pUserData)->dataCallback(output, input, frameCount);
}

uapmd::MiniAudioIODevice::MiniAudioIODevice(const std::string& deviceName) :
    config(ma_engine_config_init()),
    data(master_context, 0) {
    config.dataCallback = data_callback;
    config.periodSizeInFrames = 1024; // FIXME: provide audio buffer size
    // FIXME: support explicit device specification by `deviceName`,

    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        throw std::runtime_error("uapmd: Failed to initialize miniaudio driver.");
    }
    engine.pDevice->pUserData = this;

    auto device = ma_engine_get_device(&engine);
    data.configureMainBus(device->capture.channels, device->playback.channels, config.periodSizeInFrames);
    dataOutPtrs.clear();
    if (device->playback.channels)
        dataOutPtrs.resize(device->playback.channels);
}

uapmd::MiniAudioIODevice::~MiniAudioIODevice() {
    ma_engine_uninit(&engine);
}

uapmd_status_t uapmd::MiniAudioIODevice::start() {
    ma_engine_start(&engine);

    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

uapmd_status_t uapmd::MiniAudioIODevice::stop() {
    ma_engine_stop(&engine);

    // FIXME: maybe switch to remidy::StatusCode?
    return 0;
}

bool uapmd::MiniAudioIODevice::isPlaying() {
    return ma_device_get_state(ma_engine_get_device(&engine)) == ma_device_state_started;
}

double uapmd::MiniAudioIODevice::sampleRate() {
    return engine.pDevice->sampleRate;
}

uint32_t uapmd::MiniAudioIODevice::channels() {
    return ma_engine_get_channels(&engine);
}

void uapmd::MiniAudioIODevice::dataCallback(void *output, const void *input, ma_uint32 frameCount) {
    // audio device only has the main bus
    int32_t mainBus = 0;

    if (data.audioInBusCount() > 0) {
        // FIXME: it should be pre-allocated elsewhere
        auto inChannels = data.inputChannelCount(mainBus);
        auto inputView = choc::buffer::createChannelArrayView((float* const *) input, inChannels, frameCount);
        inputView.data.channels = (float* const *) input;
        for (size_t i = 0, n = inChannels; i < n; i++)
            memcpy(data.getFloatInBuffer(mainBus, i), inputView.getChannel(i).data.data, sizeof(float) * frameCount);
    }
    data.frameCount(frameCount);

    for (auto& callback : callbacks)
        callback(data);

    /* ... or use this for testing I/O channel remapping.
    {
        static double phase = 0.0;
        const double frequency = 440.0;  // A4 note
        const double amplitude = 0.5;    // 50% amplitude to avoid clipping

        auto bbb = data.audioOut(0);
        float *out = bbb->getFloatBufferForChannel(0);

        for (int32_t i = 0; i < data.frameCount(); ++i) {
            // Generate sine wave
            out[i] = amplitude * std::sin(phase);

            // Increment and wrap phase
            phase += 2.0 * M_PI * frequency / sampleRate();
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
        memcpy(bbb->getFloatBufferForChannel(1), out, data.frameCount() * sizeof(float));
    }*/

    if (data.audioOutBusCount() > 0) {
        // FIXME: it should be pre-allocated elsewhere
        size_t outChannels = data.outputChannelCount(mainBus);
        for (int i = 0, n = data.outputChannelCount(mainBus); i < n; i++)
            dataOutPtrs[i] = data.getFloatOutBuffer(mainBus, i);
        auto outcomeView = choc::buffer::createChannelArrayView(dataOutPtrs.data(), outChannels, frameCount);
        auto outputView = choc::buffer::createInterleavedView((float*) output, outChannels, frameCount);
        choc::buffer::copyRemappingChannels(outputView, outcomeView);
    }
}
