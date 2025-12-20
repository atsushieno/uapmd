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
}

// The entire API is hacky so far...
uapmd::AudioIODevice *uapmd::MiniAudioIODeviceManager::open(int inputDeviceIndex, int outputDeviceIndex, uint32_t sampleRate) {
    static uapmd::MiniAudioIODevice audio{this};

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
}

std::vector<uint32_t> uapmd::MiniAudioIODeviceManager::getDeviceSampleRates(const std::string& deviceName, AudioIODirections direction) {
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

    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        throw std::runtime_error("uapmd: Failed to initialize miniaudio driver.");
    }

    // Keep pUserData pointing to this device for data callback
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

    // Create a new device with the specified IDs
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_duplex);
    deviceConfig.capture.pDeviceID = inputDeviceId;
    deviceConfig.playback.pDeviceID = outputDeviceId;
    deviceConfig.capture.format = ma_format_f32;
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.sampleRate = sampleRate; // 0 = use device's native sample rate
    deviceConfig.dataCallback = data_callback;
    deviceConfig.pUserData = this;
    deviceConfig.periodSizeInFrames = config.periodSizeInFrames;
    deviceConfig.notificationCallback = config.notificationCallback;
    deviceConfig.noPreSilencedOutputBuffer = MA_TRUE;

    ma_device* pDevice = new ma_device();
    if (ma_device_init(config.pContext, &deviceConfig, pDevice) != MA_SUCCESS) {
        remidy::Logger::global()->logError("Failed to initialize device with new device IDs");
        delete pDevice;
        return false;
    }

    // Update config to use the new device
    config.pDevice = pDevice;
    config.pPlaybackDeviceID = (ma_device_id*)outputDeviceId;
    if (sampleRate > 0) {
        config.sampleRate = sampleRate;
    }

    // Reinitialize the engine
    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        remidy::Logger::global()->logError("Failed to reinitialize audio engine with new devices");
        ma_device_uninit(pDevice);
        delete pDevice;
        return false;
    }

    // Restore callbacks
    callbacks = savedCallbacks;

    // Restore pUserData
    engine.pDevice->pUserData = this;

    // Update channel counts
    auto device = ma_engine_get_device(&engine);
    input_channels = device->capture.channels;
    output_channels = device->playback.channels;
    data.configureMainBus(device->capture.channels, device->playback.channels, config.periodSizeInFrames);
    dataOutPtrs.clear();
    if (device->playback.channels)
        dataOutPtrs.resize(device->playback.channels);

    // Restart if it was running
    if (wasRunning) {
        start();
    }

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
