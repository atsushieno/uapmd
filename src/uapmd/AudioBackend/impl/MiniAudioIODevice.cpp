#define MINIAUDIO_IMPLEMENTATION 1
#include "uapmd/priv/CommonTypes.hpp"
#include "MiniAudioIODevice.hpp"
#include <choc/audio/choc_SampleBuffers.h>

// MiniAudioIODeviceManager

#undef ERROR

uapmd::Logger::LogLevel convertFromMALogLevel(uint32_t maLevel) {
    switch (maLevel) {
        case MA_LOG_LEVEL_ERROR: return uapmd::Logger::LogLevel::ERROR;
        case MA_LOG_LEVEL_WARNING: return uapmd::Logger::LogLevel::WARNING;
        case MA_LOG_LEVEL_INFO: return uapmd::Logger::LogLevel::INFO;
        case MA_LOG_LEVEL_DEBUG: return uapmd::Logger::LogLevel::DIAGNOSTIC;
    }
    // default
    return uapmd::Logger::LogLevel::INFO;
}

void uapmd::MiniAudioIODeviceManager::on_ma_log(void* userData, uint32_t logLevel, const char* message) {
    auto logger = ((uapmd::MiniAudioIODeviceManager*) userData)->remidy_logger;
    if (logger)
        logger->log(convertFromMALogLevel(logLevel), message);
}

uapmd::MiniAudioIODeviceManager::MiniAudioIODeviceManager(
        ) : AudioIODeviceManager("miniaudio") {
}

void uapmd::MiniAudioIODeviceManager::initialize(uapmd::AudioIODeviceManager::Configuration &config) {
    remidy_logger = config.logger ? config.logger : Logger::global();

    auto logCallback = ma_log_callback_init(on_ma_log, this);
    static auto allocCB = ma_allocation_callbacks_init_default();
    auto result = ma_log_init(nullptr, &ma_logger);
    if (result != MA_SUCCESS)
        Logger::global()->logError("Failed at ma_log_init");
    result = ma_log_register_callback(&ma_logger, logCallback);
    if (result != MA_SUCCESS)
        Logger::global()->logError("Failed at ma_log_register_callback.");

    auto cfg = ma_context_config_init();
    cfg.pLog = &ma_logger;
    ma_context_init(nullptr, 0, &cfg, &context);

    initialized = true;
}

const ma_device_id* findDeviceId(ma_context& context, std::string name, uapmd::AudioIODirections directions) {
    ma_device_info* playback;
    ma_device_info* capture;
    ma_uint32 playbackCount, captureCount;
    ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount);
    if (directions & uapmd::AudioIODirections::Input) {
        for (ma_uint32 i = 0; i < captureCount; i++)
            if (name == capture[i].name)
                return &capture[i].id;
    }
    if (directions & uapmd::AudioIODirections::Output) {
        for (ma_uint32 i = 0; i < playbackCount; i++)
            if (name == playback[i].name)
                return &playback[i].id;
    }
    return nullptr;
}

std::vector<uapmd::AudioIODeviceInfo> uapmd::MiniAudioIODeviceManager::onDevices() {
    ma_device_info* playback;
    ma_device_info* capture;
    ma_uint32 playbackCount, captureCount;
    ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount);
    std::vector<AudioIODeviceInfo> ret{};
    // For device IDs we treat ma_device_id as if it contained char[256]...
    for (ma_uint32 i = 0; i < captureCount; i++) {
        ret.emplace_back(AudioIODeviceInfo {
                .directions = AudioIODirections::Input,
                .id = static_cast<int32_t>(std::hash<std::string_view>{}(std::string_view(capture[i].id.custom.s))),
                .name = capture[i].name,
                .sampleRate = capture[i].nativeDataFormats[0].sampleRate,
                .channels = capture[i].nativeDataFormats[0].channels,
        });
    }
    for (ma_uint32 i = 0; i < playbackCount; i++)
        ret.emplace_back(AudioIODeviceInfo {
                .directions = AudioIODirections::Output,
                .id = static_cast<int32_t>(std::hash<std::string_view>{}(std::string_view(playback[i].id.custom.s))),
                .name = playback[i].name,
                .sampleRate = playback[i].nativeDataFormats[0].sampleRate,
                .channels = playback[i].nativeDataFormats[0].channels,
        });
    return ret;
}

// The entire API is hacky so far...
uapmd::AudioIODevice *uapmd::MiniAudioIODeviceManager::open() {
    static uapmd::MiniAudioIODevice audio{this};
    return &audio;
}

// MiniAudioIODevice

static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    ((uapmd::MiniAudioIODevice*) device->pUserData)->dataCallback(output, input, frameCount);
}

uapmd::MiniAudioIODevice::MiniAudioIODevice(
        MiniAudioIODeviceManager* manager
    ) : config(ma_engine_config_init()),
        data(master_context, 0) {
    config.pContext = &manager->maContext();
    config.dataCallback = data_callback;
    config.periodSizeInFrames = 1024; // FIXME: provide audio buffer size
    config.noAutoStart = true;

    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        throw std::runtime_error("uapmd: Failed to initialize miniaudio driver.");
    }
    engine.pDevice->pUserData = this;

    auto device = ma_engine_get_device(&engine);
    input_channels = device->capture.channels;
    output_channels = device->playback.channels;
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
    return output_channels;
}

uint32_t uapmd::MiniAudioIODevice::inputChannels() {
    return input_channels;
}

uint32_t uapmd::MiniAudioIODevice::outputChannels() {
    return output_channels;
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
