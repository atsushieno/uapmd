#define MINIAUDIO_IMPLEMENTATION 1
#include "uapmd/uapmd.hpp"
#include "MiniAudioIODevice.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
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

void uapmd::MiniAudioIODeviceManager::on_ma_device_notification(const ma_device_notification* pNotification) {
    // This callback is invoked from miniaudio when device state changes
    if (!pNotification || !pNotification->pDevice) {
        return;
    }

    auto device = static_cast<MiniAudioIODevice*>(pNotification->pDevice->pUserData);
    if (!device) {
        return;
    }

    auto manager = device->getManager();
    if (!manager) {
        return;
    }

    // Device ID would need proper mapping from ma_device_id
    int32_t deviceId = 0;

    switch (pNotification->type) {
        case ma_device_notification_type_started:
            // Device has started - could indicate new device available
            manager->notifyDeviceChange(deviceId, UAPMD_AUDIO_DEVICE_CHANGE_ADDED);
            break;
        case ma_device_notification_type_stopped:
        case ma_device_notification_type_rerouted:
            // Device stopped or rerouted - might indicate device removal
            manager->notifyDeviceChange(deviceId, UAPMD_AUDIO_DEVICE_CHANGE_REMOVED);
            break;
        default:
            break;
    }
}

uapmd::MiniAudioIODeviceManager::MiniAudioIODeviceManager(
        ) : AudioIODeviceManager("miniaudio") {
}

uapmd::MiniAudioIODeviceManager::~MiniAudioIODeviceManager() {
    ma_context_uninit(&context);
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
    if (directions & uapmd::UAPMD_AUDIO_DIRECTION_INPUT) {
        for (ma_uint32 i = 0; i < captureCount; i++)
            if (name == capture[i].name)
                return &capture[i].id;
    }
    if (directions & uapmd::UAPMD_AUDIO_DIRECTION_OUTPUT) {
        for (ma_uint32 i = 0; i < playbackCount; i++)
            if (name == playback[i].name)
                return &playback[i].id;
    }
    return nullptr;
}

std::vector<uapmd::AudioIODeviceInfo> uapmd::MiniAudioIODeviceManager::onDevices() {
#if defined(__EMSCRIPTEN__)
    // WebAudio backend exposes a single playback device with fixed format.
    return { AudioIODeviceInfo{
            .directions = UAPMD_AUDIO_DIRECTION_OUTPUT,
            .id = 0,
            .name = "Web Audio",
            .sampleRate = 48000,
            .channels = 2,
    }};
#else
    ma_device_info* playback;
    ma_device_info* capture;
    ma_uint32 playbackCount, captureCount;
    ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount);
    std::vector<AudioIODeviceInfo> ret{};
    // For device IDs we treat ma_device_id as if it contained char[256]...
    for (ma_uint32 i = 0; i < captureCount; i++) {
        ret.emplace_back(AudioIODeviceInfo {
                .directions = UAPMD_AUDIO_DIRECTION_INPUT,
                .id = static_cast<int32_t>(std::hash<std::string_view>{}(std::string_view(capture[i].id.custom.s))),
                .name = capture[i].name,
                .sampleRate = capture[i].nativeDataFormats[0].sampleRate,
                .channels = capture[i].nativeDataFormats[0].channels,
        });
    }
    for (ma_uint32 i = 0; i < playbackCount; i++) {
        ret.emplace_back(AudioIODeviceInfo {
                .directions = UAPMD_AUDIO_DIRECTION_OUTPUT,
                .id = static_cast<int32_t>(std::hash<std::string_view>{}(std::string_view(playback[i].id.custom.s))),
                .name = playback[i].name,
                .sampleRate = playback[i].nativeDataFormats[0].sampleRate,
                .channels = playback[i].nativeDataFormats[0].channels,
        });
    }
    return ret;
#endif
}

// The entire API is hacky so far...
uapmd::AudioIODevice *uapmd::MiniAudioIODeviceManager::onOpen(int inputDeviceIndex, int outputDeviceIndex, uint32_t sampleRate) {
    static uapmd::MiniAudioIODevice audio{this};

#if defined(__EMSCRIPTEN__)
    // Web builds always use the default playback device exposed via WebAudio.
    if (!audio.reconfigure(nullptr, nullptr, sampleRate)) {
        remidy_logger->logError("Failed to configure Web Audio device");
        return nullptr;
    }
    return &audio;
#else
    // Get device lists
    ma_device_info* playbackDevices;
    ma_device_info* captureDevices;
    ma_uint32 playbackCount, captureCount;
    ma_context_get_devices(&context, &playbackDevices, &playbackCount, &captureDevices, &captureCount);

    // Find device IDs by index (-1 means use default device, i.e., nullptr)
    const ma_device_id* inputId = nullptr;
    const ma_device_id* outputId = nullptr;

    if (inputDeviceIndex >= 0 && static_cast<ma_uint32>(inputDeviceIndex) < captureCount) {
        inputId = &captureDevices[inputDeviceIndex].id;
    } else if (inputDeviceIndex >= 0) {
        remidy_logger->logWarning("Input device index {} out of range (max {}), using default", inputDeviceIndex, captureCount - 1);
    }

    if (outputDeviceIndex >= 0 && static_cast<ma_uint32>(outputDeviceIndex) < playbackCount) {
        outputId = &playbackDevices[outputDeviceIndex].id;
    } else if (outputDeviceIndex >= 0) {
        remidy_logger->logWarning("Output device index {} out of range (max {}), using default", outputDeviceIndex, playbackCount - 1);
    }

    // Reconfigure the device with the new IDs and sample rate
    if (!audio.reconfigure(inputId, outputId, sampleRate)) {
        remidy_logger->logError("Failed to reconfigure audio device");
        return nullptr;
    }

    return &audio;
#endif
}

std::vector<uint32_t> uapmd::MiniAudioIODeviceManager::getDeviceSampleRates(const std::string& deviceName, AudioIODirections direction) {
#if defined(__EMSCRIPTEN__)
    // WebAudio currently exposes a fixed-rate playback path (normally 48kHz).
    constexpr uint32_t kWebAudioRate = 48000;
    return {kWebAudioRate};
#else
    std::vector<uint32_t> sampleRates;

    ma_device_info* playback;
    ma_device_info* capture;
    ma_uint32 playbackCount, captureCount;
    ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount);

    if (direction & UAPMD_AUDIO_DIRECTION_INPUT) {
        for (ma_uint32 i = 0; i < captureCount; i++) {
            if (deviceName == capture[i].name) {
                // Use ma_context_get_device_info to get detailed device information
                ma_device_info deviceInfo;
                ma_result result = ma_context_get_device_info(&context, ma_device_type_capture, &capture[i].id, &deviceInfo);
                if (result == MA_SUCCESS) {
                    for (ma_uint32 j = 0; j < deviceInfo.nativeDataFormatCount; j++) {
                        sampleRates.push_back(deviceInfo.nativeDataFormats[j].sampleRate);
                    }
                }
                return sampleRates;
            }
        }
    }

    if (direction & UAPMD_AUDIO_DIRECTION_OUTPUT) {
        for (ma_uint32 i = 0; i < playbackCount; i++) {
            if (deviceName == playback[i].name) {
                // Use ma_context_get_device_info to get detailed device information
                ma_device_info deviceInfo;
                ma_result result = ma_context_get_device_info(&context, ma_device_type_playback, &playback[i].id, &deviceInfo);
                if (result == MA_SUCCESS) {
                    for (ma_uint32 j = 0; j < deviceInfo.nativeDataFormatCount; j++) {
                        sampleRates.push_back(deviceInfo.nativeDataFormats[j].sampleRate);
                    }
                }
                return sampleRates;
            }
        }
    }

    return sampleRates;
#endif
}

// MiniAudioIODevice

static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    ((uapmd::MiniAudioIODevice*) device->pUserData)->dataCallback(output, input, frameCount);
}

uapmd::MiniAudioIODevice::MiniAudioIODevice(
        MiniAudioIODeviceManager* manager
    ) : config(ma_engine_config_init()),
        data(master_context, 0),
        manager_(manager) {
    config.pContext = &manager->maContext();
    config.dataCallback = data_callback;
    config.periodSizeInFrames = 1024; // FIXME: provide audio buffer size
    config.noAutoStart = true;
    config.notificationCallback = MiniAudioIODeviceManager::on_ma_device_notification;

    if (!initializeDuplexDevice(nullptr, nullptr, 0)) {
        throw std::runtime_error("uapmd: Failed to initialize miniaudio driver.");
    }
}

uapmd::MiniAudioIODevice::~MiniAudioIODevice() {
    // Ensure the engine is fully stopped before uninitializing
    // This prevents race conditions where the audio callback might still be running
    if (isPlaying()) {
        stop();
    }

    // Wait for any pending audio callbacks to complete
    // The ma_engine_uninit will stop the underlying device, but we need to ensure
    // callbacks are done before we destroy the AudioProcessContext member
    ma_device* device = ma_engine_get_device(&engine);
    if (device) {
        ma_device_stop(device);
        // Give the device time to fully stop and drain callbacks
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ma_engine_uninit(&engine);
}

bool uapmd::MiniAudioIODevice::reconfigure(const ma_device_id* inputDeviceId, const ma_device_id* outputDeviceId, uint32_t sampleRate) {
    // Stop the engine if it's running
    bool wasRunning = isPlaying();
    if (wasRunning) {
        stop();
    }

    // Save callbacks before uninitializing
    auto savedCallbacks = callbacks;

    // Uninitialize the current engine
    ma_engine_uninit(&engine);

    // Reinitialize the engine with the requested device configuration
    if (!initializeDuplexDevice(inputDeviceId, outputDeviceId, sampleRate)) {
        return false;
    }

    // Restore callbacks
    callbacks = savedCallbacks;

    // Restart if it was running
    if (wasRunning) {
        start();
    }

    return true;
}

bool uapmd::MiniAudioIODevice::initializeDuplexDevice(const ma_device_id* inputDeviceId, const ma_device_id* outputDeviceId, uint32_t sampleRate) {
#if ANDROID || defined(__EMSCRIPTEN__)
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
#else
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_duplex);
    deviceConfig.capture.pDeviceID = inputDeviceId;
    deviceConfig.capture.format = ma_format_f32;
#endif
    deviceConfig.playback.pDeviceID = outputDeviceId;
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.sampleRate = sampleRate; // 0 = native sample rate
    deviceConfig.dataCallback = data_callback;
    deviceConfig.pUserData = this;
    deviceConfig.periodSizeInFrames = config.periodSizeInFrames;
    deviceConfig.notificationCallback = config.notificationCallback;
    deviceConfig.noPreSilencedOutputBuffer = MA_TRUE;

    auto* newDevice = new ma_device();
    if (ma_device_init(config.pContext, &deviceConfig, newDevice) != MA_SUCCESS) {
        remidy::Logger::global()->logError("Failed to initialize audio device");
        delete newDevice;
        return false;
    }

    config.pDevice = newDevice;
    config.pPlaybackDeviceID = const_cast<ma_device_id*>(outputDeviceId);
    if (sampleRate > 0) {
        config.sampleRate = sampleRate;
    }

    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        remidy::Logger::global()->logError("Failed to initialize audio engine");
        ma_device_uninit(newDevice);
        delete newDevice;
        config.pDevice = nullptr;
        return false;
    }

    engine.pDevice->pUserData = this;

    auto device = ma_engine_get_device(&engine);
#if defined(__EMSCRIPTEN__)
    input_channels = 0;
#else
    input_channels = device->capture.channels;
#endif
    output_channels = device->playback.channels;
    data.configureMainBus(device->capture.channels, device->playback.channels, config.periodSizeInFrames);
    dataOutPtrs.clear();
    if (device->playback.channels)
        dataOutPtrs.resize(device->playback.channels);

    return true;
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

std::vector<uint32_t> uapmd::MiniAudioIODevice::getNativeSampleRates() {
    std::vector<uint32_t> sampleRates;
    auto device = ma_engine_get_device(&engine);

    if (!device) {
        return sampleRates;
    }

#if defined(__EMSCRIPTEN__)
    sampleRates.push_back(device->sampleRate);
    return sampleRates;
#endif

    // Get device info for the opened device
    ma_device_info deviceInfo;
    ma_result result;

    if (device->type == ma_device_type_playback) {
        result = ma_context_get_device_info(device->pContext, ma_device_type_playback, &device->playback.id, &deviceInfo);
    } else if (device->type == ma_device_type_capture) {
        result = ma_context_get_device_info(device->pContext, ma_device_type_capture, &device->capture.id, &deviceInfo);
    } else {
        // Duplex - get playback info (could also get capture)
        result = ma_context_get_device_info(device->pContext, ma_device_type_playback, &device->playback.id, &deviceInfo);
    }

    if (result == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < deviceInfo.nativeDataFormatCount; i++) {
            sampleRates.push_back(deviceInfo.nativeDataFormats[i].sampleRate);
        }
    }

    return sampleRates;
}

void uapmd::MiniAudioIODevice::dataCallback(void *output, const void *input, ma_uint32 frameCount) {
    // audio device only has the main bus
    int32_t mainBus = 0;

    if (data.audioInBusCount() > 0 && input) {
        const size_t pluginChannels = static_cast<size_t>(data.inputChannelCount(mainBus));
        const size_t hardwareChannels = static_cast<size_t>(input_channels);
        const size_t mappedChannels = std::min(pluginChannels, hardwareChannels);

        static thread_local std::vector<float*> inputChannelPtrs{};
        inputChannelPtrs.resize(mappedChannels);

        if (mappedChannels > 0) {
            for (size_t i = 0; i < mappedChannels; ++i)
                inputChannelPtrs[i] = data.getFloatInBuffer(mainBus, static_cast<uint32_t>(i));

            auto pluginView = choc::buffer::createChannelArrayView(inputChannelPtrs.data(), mappedChannels, frameCount);
            auto deviceView = choc::buffer::createInterleavedView(static_cast<const float*>(input), hardwareChannels, frameCount);
            choc::buffer::copyRemappingChannels(pluginView, deviceView);
        }

        for (size_t ch = mappedChannels; ch < pluginChannels; ++ch) {
            auto* dst = data.getFloatInBuffer(mainBus, static_cast<uint32_t>(ch));
            if (dst)
                std::fill(dst, dst + frameCount, 0.0f);
        }
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
