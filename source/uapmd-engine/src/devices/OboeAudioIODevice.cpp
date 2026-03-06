#if defined(__ANDROID__)

#include "OboeAudioIODevice.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <strings.h>
#include <sys/system_properties.h>

using namespace oboe;

namespace uapmd {

    OboeAudioIODevice::OboeAudioIODevice(Logger* logger)
        : logger_(logger ? logger : Logger::global()),
          data(master_context, 0) {}

    OboeAudioIODevice::~OboeAudioIODevice() {
        stop();
    }

    void OboeAudioIODevice::addAudioCallback(std::function<uapmd_status_t(AudioProcessContext&)> &&callback) {
        callbacks.emplace_back(std::move(callback));
    }

    void OboeAudioIODevice::clearAudioCallbacks() {
        callbacks.clear();
    }

    std::vector<uint32_t> OboeAudioIODevice::getNativeSampleRates() {
        if (stream_)
            return { static_cast<uint32_t>(stream_->getSampleRate()) };
        return { requested_sample_rate_, 48000 };
    }

    bool OboeAudioIODevice::reconfigure(uint32_t sampleRateHint, uint32_t channelCountHint, uint32_t bufferSizeHint) {
        if (sampleRateHint > 0)
            requested_sample_rate_ = sampleRateHint;
        if (channelCountHint > 0)
            requested_output_channels_ = channelCountHint;
        requested_buffer_size_ = bufferSizeHint;
        if (isPlaying())
            stop();
        return true;
    }

    namespace {
        const char* toString(AudioApi api) {
            switch (api) {
                case AudioApi::AAudio: return "AAudio";
                case AudioApi::OpenSLES: return "OpenSLES";
                default: return "Unspecified";
            }
        }

        const char* toString(SharingMode mode) {
            switch (mode) {
                case SharingMode::Exclusive: return "Exclusive";
                case SharingMode::Shared: return "Shared";
                default: return "Unknown";
            }
        }

        const char* toString(PerformanceMode mode) {
            switch (mode) {
                case PerformanceMode::None: return "None";
                case PerformanceMode::PowerSaving: return "PowerSaving";
                case PerformanceMode::LowLatency: return "LowLatency";
                default: return "Unknown";
            }
        }

        bool isProbablyEmulator() {
            char prop[PROP_VALUE_MAX]{};
            if (__system_property_get("ro.kernel.qemu", prop) > 0) {
                if (prop[0] == '1')
                    return true;
            }
            if (__system_property_get("ro.hardware", prop) > 0) {
                if (strstr(prop, "ranchu") || strstr(prop, "goldfish"))
                    return true;
            }
            if (__system_property_get("ro.product.manufacturer", prop) > 0) {
                if (strcasestr(prop, "generic"))
                    return true;
            }
            return false;
        }
    }

    bool OboeAudioIODevice::openStream() {
        if (stream_)
            return true;

        // Do NOT call setFramesPerCallback — let Oboe pick the optimal
        // callback size based on the hardware's framesPerBurst.
        // The chunking loop in onAudioReady handles any mismatch with
        // our internal buffer capacity.
        auto attemptOpen = [&](bool exclusive) -> bool {
            AudioStreamBuilder builder;
            builder.setDirection(Direction::Output);
            builder.setPerformanceMode(exclusive ? PerformanceMode::LowLatency : PerformanceMode::None);
            builder.setSharingMode(exclusive ? SharingMode::Exclusive : SharingMode::Shared);
            builder.setFormat(AudioFormat::Float);
            builder.setFormatConversionAllowed(true);
            builder.setChannelConversionAllowed(true);
            builder.setSampleRateConversionQuality(SampleRateConversionQuality::Medium);
            builder.setChannelCount(requested_output_channels_);
            builder.setCallback(this);
            builder.setErrorCallback(this);
            builder.setUsage(Usage::Media);
            builder.setContentType(ContentType::Music);
            if (requested_sample_rate_ > 0)
                builder.setSampleRate(requested_sample_rate_);

            AudioStream* rawStream = nullptr;
            const auto result = builder.openStream(&rawStream);
            if (result != Result::OK) {
                logger_->logWarning("OboeAudioIODevice: openStream (exclusive=%d) failed: %s",
                                    exclusive ? 1 : 0, convertToText(result));
                return false;
            }
            stream_.reset(rawStream);
            return true;
        };

        const bool runningOnEmulator = isProbablyEmulator();
        bool opened = false;
        if (!runningOnEmulator)
            opened = attemptOpen(true);
        if (!opened)
            opened = attemptOpen(false);
        if (!opened) {
            logger_->logError("OboeAudioIODevice: failed to open any audio stream (emulator=%d)", runningOnEmulator ? 1 : 0);
            return false;
        }

        sample_rate_ = static_cast<double>(stream_->getSampleRate());
        output_channels_ = static_cast<uint32_t>(stream_->getChannelCount());
        const auto framesPerBurst = stream_->getFramesPerBurst() > 0
            ? static_cast<uint32_t>(stream_->getFramesPerBurst())
            : 0u;

        // Internal buffer capacity: sized to handle the engine's processing
        // block size (requested_buffer_size_).  Falls back to a multiple of
        // framesPerBurst or 512 when the caller didn't specify a size.
        if (requested_buffer_size_ > 0)
            buffer_capacity_frames_ = requested_buffer_size_;
        else if (framesPerBurst > 0)
            buffer_capacity_frames_ = framesPerBurst * 2u;
        else
            buffer_capacity_frames_ = 512u;

        master_context.sampleRate(static_cast<int32_t>(sample_rate_));
        data.configureMainBus(static_cast<int32_t>(input_channels_),
                              static_cast<int32_t>(output_channels_),
                              buffer_capacity_frames_);
        dataOutPtrs.resize(output_channels_);

        // Use requested_buffer_size_ as a latency hint via setBufferSizeInFrames
        // (the Oboe ring-buffer between app and hardware).
        // This is clamped to [0, bufferCapacityInFrames] by Oboe automatically.
        uint32_t appliedBufferSize = 0;
        if (requested_buffer_size_ > 0) {
            const auto setResult = stream_->setBufferSizeInFrames(
                static_cast<int32_t>(requested_buffer_size_));
            if (setResult)
                appliedBufferSize = static_cast<uint32_t>(std::max(setResult.value(), 0));
            else
                logger_->logWarning("OboeAudioIODevice: setBufferSizeInFrames(%u) failed: %s",
                                    requested_buffer_size_,
                                    convertToText(setResult.error()));
        }

        const auto hardwareCapacity = stream_->getBufferCapacityInFrames() > 0
            ? static_cast<uint32_t>(stream_->getBufferCapacityInFrames())
            : 0u;

        logger_->logInfo(
            "OboeAudioIODevice: opened stream api=%s sharing=%s perf=%s sr=%d ch=%d "
            "framesPerBurst=%d internalCapacity=%u requestedLatency=%u appliedLatency=%u hwCapacity=%u",
            toString(stream_->getAudioApi()),
            toString(stream_->getSharingMode()),
            toString(stream_->getPerformanceMode()),
            stream_->getSampleRate(),
            stream_->getChannelCount(),
            stream_->getFramesPerBurst(),
            buffer_capacity_frames_,
            requested_buffer_size_,
            appliedBufferSize,
            hardwareCapacity);
        return true;
    }

    void OboeAudioIODevice::closeStream() {
        if (stream_) {
            stream_->close();
            stream_.reset();
        }
    }

    uapmd_status_t OboeAudioIODevice::start() {
        if (!openStream())
            return -1;
        const auto result = stream_->requestStart();
        if (result != Result::OK) {
            logger_->logError("OboeAudioIODevice: requestStart failed: %s", convertToText(result));
            closeStream();
            return -1;
        }
        playing_ = true;
        return 0;
    }

    uapmd_status_t OboeAudioIODevice::stop() {
        if (stream_) {
            stream_->requestStop();
            closeStream();
        }
        playing_ = false;
        return 0;
    }

    DataCallbackResult OboeAudioIODevice::onAudioReady(AudioStream *audioStream,
                                                       void *audioData,
                                                       int32_t numFrames) {
        if (!audioData || numFrames <= 0)
            return DataCallbackResult::Stop;

        const int32_t capacity = static_cast<int32_t>(data.audioBufferCapacityInFrames());
        if (capacity <= 0)
            return DataCallbackResult::Stop;

        auto* out = static_cast<float*>(audioData);
        const size_t hardwareChannels = static_cast<size_t>(audioStream->getChannelCount());
        if (hardwareChannels == 0)
            return DataCallbackResult::Continue;
        if (dataOutPtrs.size() < hardwareChannels)
            dataOutPtrs.resize(hardwareChannels);

        int32_t framesProcessed = 0;
        while (framesProcessed < numFrames) {
            const int32_t framesThisBlock = std::min(capacity, numFrames - framesProcessed);
            data.frameCount(framesThisBlock);

            // Zero outputs before handing to callbacks
            if (data.audioOutBusCount() > 0) {
                const uint32_t outChannels = static_cast<uint32_t>(data.outputChannelCount(0));
                for (uint32_t ch = 0; ch < outChannels; ++ch) {
                    if (float* dst = data.getFloatOutBuffer(0, ch))
                        std::fill_n(dst, framesThisBlock, 0.0f);
                }
            }

            for (auto& cb : callbacks)
                cb(data);

            const size_t pluginChannels = data.audioOutBusCount() > 0
                ? static_cast<size_t>(data.outputChannelCount(0))
                : 0;
            const size_t mappedChannels = std::min(pluginChannels, hardwareChannels);

            if (pluginChannels > hardwareChannels && hardwareChannels > 0) {
                if (auto* mixDown = data.getFloatOutBuffer(0, 0)) {
                    for (size_t ch = hardwareChannels; ch < pluginChannels; ++ch) {
                        if (auto* extra = data.getFloatOutBuffer(0, static_cast<uint32_t>(ch))) {
                            for (int32_t frame = 0; frame < framesThisBlock; ++frame)
                                mixDown[frame] += extra[frame];
                        }
                    }
                }
            }

            for (size_t ch = 0; ch < hardwareChannels; ++ch) {
                dataOutPtrs[ch] = (ch < mappedChannels)
                    ? data.getFloatOutBuffer(0, static_cast<uint32_t>(ch))
                    : nullptr;
            }

            for (int32_t frame = 0; frame < framesThisBlock; ++frame) {
                for (size_t ch = 0; ch < hardwareChannels; ++ch) {
                    float sample = 0.0f;
                    if (auto* src = dataOutPtrs[ch])
                        sample = src[frame];
                    out[(framesProcessed + frame) * hardwareChannels + ch] = sample;
                }
            }

            framesProcessed += framesThisBlock;
        }

        return DataCallbackResult::Continue;
    }

    bool OboeAudioIODevice::onError(AudioStream *audioStream, Result error) {
        (void) audioStream;
        logger_->logError("OboeAudioIODevice: stream error %s", convertToText(error));
        playing_ = false;
        closeStream();
        return false;
    }

    void OboeAudioIODeviceManager::initialize(Configuration &config) {
        logger_ = config.logger ? config.logger : Logger::global();
        if (!audio_)
            audio_ = std::make_unique<OboeAudioIODevice>(logger_);
        initialized = true;
    }

    std::vector<AudioIODeviceInfo> OboeAudioIODeviceManager::onDevices() {
        AudioIODeviceInfo info{
            .directions = UAPMD_AUDIO_DIRECTION_OUTPUT,
            .id = 0,
            .name = "Oboe (AAudio)",
            .sampleRate = audio_ ? static_cast<uint32_t>(audio_->sampleRate()) : 48000u,
            .channels = audio_ ? audio_->outputChannels() : 2u
        };
        return { info };
    }

    AudioIODevice* OboeAudioIODeviceManager::onOpen(int inputDeviceIndex,
                                                    int outputDeviceIndex,
                                                    uint32_t sampleRate,
                                                    uint32_t bufferSize) {
        (void) inputDeviceIndex;
        (void) outputDeviceIndex;
        if (!audio_)
            audio_ = std::make_unique<OboeAudioIODevice>(logger_);
        audio_->reconfigure(sampleRate, 2, bufferSize);
        return audio_.get();
    }

    std::vector<uint32_t> OboeAudioIODeviceManager::getDeviceSampleRates(const std::string &deviceName,
                                                                         AudioIODirections direction) {
        (void) deviceName;
        (void) direction;
        if (audio_)
            return audio_->getNativeSampleRates();
        return {48000};
    }
}

#endif // __ANDROID__
