#define MINIAUDIO_IMPLEMENTATION 1
#include "MiniAudioIODevice.hpp"
#include <choc/audio/choc_SampleBuffers.h>

// MiniAudioIODeviceManager

void uapmd::MiniAudioIODeviceManager::on_ma_log(void* userData, uint32_t logLevel, const char* message) {
    auto logger = ((uapmd::MiniAudioIODeviceManager*) userData)->remidy_logger;
    if (logger)
        logger->log((remidy::Logger::LogLevel) logLevel, message);
}

uapmd::MiniAudioIODeviceManager::MiniAudioIODeviceManager(
        ) : AudioIODeviceManager("miniaudio") {
}

void uapmd::MiniAudioIODeviceManager::initialize(uapmd::AudioIODeviceManager::Configuration &config) {
    remidy_logger = config.logger;

    size_t backendCount;
    ma_get_enabled_backends(nullptr, 0, &backendCount);
    backends.resize(backendCount);

    auto logCallback = ma_log_callback_init(on_ma_log, this);
    ma_log_init(nullptr, &ma_logger);
    ma_log_register_callback(&ma_logger, logCallback);

    ma_get_enabled_backends(backends.data(), backendCount, nullptr);
    auto cfg = ma_context_config_init();
    cfg.pLog = &ma_logger;
    ma_context_init(backends.data(), backendCount, &cfg, &context);
}

std::vector<uapmd::AudioIODeviceInfo> uapmd::MiniAudioIODeviceManager::onDevices() {
    ma_device_info* playback;
    ma_device_info* capture;
    ma_uint32 playbackCount, captureCount;
    ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount);
    std::vector<AudioIODeviceInfo> ret{};
    for (ma_uint32 i = 0; i < captureCount; i++)
        ret.emplace_back(AudioIODeviceInfo {
            .directions = AudioIODeviceInfo::IODirections::Input,
            .name = capture[i].name,
            .sampleRate = capture[i].nativeDataFormats[0].sampleRate,
            .channels = capture[i].nativeDataFormats[0].channels,
        });
    // There is no duplex devices in miniaudio land, so do not consider overlaps.
    for (ma_uint32 i = 0; i < playbackCount; i++)
        ret.emplace_back(AudioIODeviceInfo {
                .directions = AudioIODeviceInfo::IODirections::Output,
                .name = playback[i].name,
                .sampleRate = playback[i].nativeDataFormats[0].sampleRate,
                .channels = playback[i].nativeDataFormats[0].channels,
        });
    return ret;
}

// The entire API is hacky so far...
uapmd::AudioIODevice *uapmd::MiniAudioIODeviceManager::activeDefaultDevice() {
    static MiniAudioIODevice device{""};
    return &device;
}

// MiniAudioIODevice

static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    ((uapmd::MiniAudioIODevice*) device->pUserData)->dataCallback(output, input, frameCount);
}

uapmd::MiniAudioIODevice::MiniAudioIODevice(
        const std::string& deviceName
    ) : config(ma_engine_config_init()),
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
