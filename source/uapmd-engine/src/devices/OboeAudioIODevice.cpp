#if defined(__ANDROID__)

#include "OboeAudioIODevice.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <strings.h>
#include <sys/system_properties.h>

using namespace oboe;

namespace uapmd {

    // Background: current AAP instrument hosting expects the DAW/host to render a fixed
    // block size per callback. Some plugins (and AAP’s MIDI plumbing) assume each call
    // produces exactly that many frames, so we keep the sequencer block length equal to
    // the user/UI “buffer size” even when the HAL reports a different framesPerBurst.
    // Oboe still decides the hardware callback cadence; we bridge the gap by rendering
    // the host-sized block into a FIFO (stabilized_buffer_) and draining however many
    // frames the HAL asks for. This compromises between AAP’s current expectations and
    // devices that require small bursts while preserving acceptable CPU load.
    //
    // Oboe's stabilized callback is an anti-pattern and not recommended, so we will
    // end up making changes to the AAP processing model. At the same time, we could
    // make use of `oboe::AudioStreamPartialDataCallback` once Android 17 is set in stone.

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
        stabilized_buffer_.clear();
        stabilized_render_scratch_.clear();
        stabilized_buffered_frames_ = 0;
    }

    void OboeAudioIODevice::setPreferredCallbackSize(uint32_t framesPerCallback) {
        preferred_callback_frames_ = framesPerCallback;
        stabilized_buffer_.clear();
        stabilized_render_scratch_.clear();
        stabilized_buffered_frames_ = 0;
        stabilized_block_frames_ = framesPerCallback;
    }

    std::vector<uint32_t> OboeAudioIODevice::getNativeSampleRates() {
        if (stream_)
            return { static_cast<uint32_t>(stream_->getSampleRate()) };
        return { requested_sample_rate_, 48000 };
    }

    bool OboeAudioIODevice::useAutoBufferSize() {
        return auto_buffer_size_;
    }

    bool OboeAudioIODevice::useAutoBufferSize(bool value) {
        auto_buffer_size_ = value;
        return auto_buffer_size_;
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
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_)
            return true;

        // We deliberately avoid setFramesPerCallback() so the HAL can choose whatever
        // framesPerBurst it needs. The sequencer always renders the host-requested
        // block size into an internal FIFO and we drain that FIFO inside onAudioReady(),
        // so hardware callbacks can be smaller or larger without forcing plugin changes.
        auto attemptOpen = [&](SharingMode sharing, PerformanceMode perf) -> bool {
            AudioStreamBuilder builder;
            builder.setDirection(Direction::Output);
            builder.setPerformanceMode(perf);
            builder.setSharingMode(sharing);
            builder.setFormat(AudioFormat::Float);
            builder.setFormatConversionAllowed(true);
            builder.setChannelConversionAllowed(true);
            builder.setSampleRateConversionQuality(SampleRateConversionQuality::Medium);
            builder.setChannelCount(requested_output_channels_);
            builder.setDataCallback(this);
            builder.setErrorCallback(this);
            builder.setUsage(Usage::Media);
            builder.setContentType(ContentType::Music);
            if (requested_sample_rate_ > 0)
                builder.setSampleRate(requested_sample_rate_);

            AudioStream* rawStream = nullptr;
            const auto result = builder.openStream(&rawStream);
            if (result != Result::OK) {
                logger_->logWarning("OboeAudioIODevice: openStream (sharing=%s perf=%s) failed: %s",
                                    toString(sharing), toString(perf), convertToText(result));
                if (rawStream)
                    rawStream->close();
                return false;
            }
            stream_.reset(rawStream);
            return true;
        };

        const bool runningOnEmulator = isProbablyEmulator();
        bool opened = false;
        if (!runningOnEmulator)
            opened = attemptOpen(SharingMode::Shared, PerformanceMode::LowLatency);
        if (!opened)
            opened = attemptOpen(SharingMode::Shared, PerformanceMode::None);
        if (!opened) {
            logger_->logError("OboeAudioIODevice: failed to open any audio stream (emulator=%d)", runningOnEmulator ? 1 : 0);
            return false;
        }

        sample_rate_ = static_cast<double>(stream_->getSampleRate());
        output_channels_ = static_cast<uint32_t>(stream_->getChannelCount());
        const auto framesPerBurst = stream_->getFramesPerBurst() > 0
            ? static_cast<uint32_t>(stream_->getFramesPerBurst())
            : 0u;
        if (!needsStabilizedMode() && framesPerBurst > 0)
            stabilized_block_frames_ = framesPerBurst;

        // Internal buffer capacity: if the host asked for a stabilized callback size
        // we need to hold at least that many frames in the FIFO. Otherwise we fall
        // back to either the user-requested HAL buffer size or a multiple of the
        // hardware framesPerBurst (defaulting to 512 when unknown).
        const bool manualBufferRequest = !auto_buffer_size_ && requested_buffer_size_ > 0;
        const uint32_t fallbackFrames = manualBufferRequest
            ? requested_buffer_size_
            : (framesPerBurst > 0 ? framesPerBurst * 2u : 512u);
        if (needsStabilizedMode())
            buffer_capacity_frames_ = std::max(fallbackFrames, stabilized_block_frames_);
        else
            buffer_capacity_frames_ = fallbackFrames;

        master_context.sampleRate(static_cast<int32_t>(sample_rate_));
        data.configureMainBus(static_cast<int32_t>(input_channels_),
                              static_cast<int32_t>(output_channels_),
                              buffer_capacity_frames_);
        dataOutPtrs.resize(output_channels_);
        stabilized_buffer_.clear();
        stabilized_render_scratch_.clear();
        stabilized_buffered_frames_ = 0;

        // Set Oboe ring-buffer (latency buffer between app and hardware).
        // Must be at least 2 * framesPerBurst for reliable playback — using
        // only the user's requested_buffer_size_ can starve the hardware
        // when framesPerBurst > requested_buffer_size_.
        const uint32_t minLatencyFrames = framesPerBurst > 0 ? framesPerBurst * 2u : 512u;
        const uint32_t latencyFrames = manualBufferRequest
            ? std::max(requested_buffer_size_, minLatencyFrames)
            : minLatencyFrames;
        uint32_t appliedBufferSize = 0;
        if (manualBufferRequest) {
            const auto setResult = stream_->setBufferSizeInFrames(
                static_cast<int32_t>(latencyFrames));
            if (setResult)
                appliedBufferSize = static_cast<uint32_t>(std::max(setResult.value(), 0));
            else
                logger_->logWarning("OboeAudioIODevice: setBufferSizeInFrames(%u) failed: %s",
                                    latencyFrames,
                                    convertToText(setResult.error()));
        }

        const auto hardwareCapacity = stream_->getBufferCapacityInFrames() > 0
            ? static_cast<uint32_t>(stream_->getBufferCapacityInFrames())
            : 0u;

        logger_->logInfo(
            "OboeAudioIODevice: opened stream api=%s sharing=%s perf=%s sr=%d ch=%d "
            "framesPerBurst=%d internalCapacity=%u stabilizedBlock=%u userBufferSize=%u latencyHint=%u appliedLatency=%u hwCapacity=%u",
            toString(stream_->getAudioApi()),
            toString(stream_->getSharingMode()),
            toString(stream_->getPerformanceMode()),
            stream_->getSampleRate(),
            stream_->getChannelCount(),
            stream_->getFramesPerBurst(),
            buffer_capacity_frames_,
            stabilized_block_frames_,
            manualBufferRequest ? requested_buffer_size_ : 0u,
            latencyFrames,
            appliedBufferSize,
            hardwareCapacity);
        return true;
    }

    void OboeAudioIODevice::closeStream() {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) {
            stream_->close();
            stream_.reset();
        }
    }

    uapmd_status_t OboeAudioIODevice::start() {
        if (!openStream())
            return -1;
        primeStabilizedBuffer();
        should_restart_after_error_.store(true);
        const auto result = stream_->requestStart();
        if (result != Result::OK) {
            logger_->logError("OboeAudioIODevice: requestStart failed: %s", convertToText(result));
            should_restart_after_error_.store(false);
            closeStream();
            return -1;
        }
        playing_ = true;
        return 0;
    }

    uapmd_status_t OboeAudioIODevice::stop() {
        should_restart_after_error_.store(false);
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) {
            const auto stopResult = stream_->stop();
            if (stopResult != Result::OK)
                logger_->logWarning("OboeAudioIODevice: stop failed: %s", convertToText(stopResult));
            stream_->close();
            stream_.reset();
        }
        playing_ = false;
        return 0;
    }

    DataCallbackResult OboeAudioIODevice::onAudioReady(AudioStream *audioStream,
                                                       void *audioData,
                                                       int32_t numFrames) {
        if (!audioData || numFrames <= 0)
            return DataCallbackResult::Stop;

        if (preferred_callback_frames_ == 0)
            return processImmediate(audioStream, static_cast<float*>(audioData), numFrames);
        return processStabilized(audioStream, static_cast<float*>(audioData), numFrames);
    }

    DataCallbackResult OboeAudioIODevice::processImmediate(AudioStream* audioStream,
                                                           float* audioData,
                                                           int32_t numFrames) {
        const int32_t capacity = static_cast<int32_t>(data.audioBufferCapacityInFrames());
        if (capacity <= 0)
            return DataCallbackResult::Stop;

        auto* out = audioData;
        const size_t hardwareChannels = static_cast<size_t>(audioStream->getChannelCount());
        if (hardwareChannels == 0)
            return DataCallbackResult::Continue;
        if (dataOutPtrs.size() < hardwareChannels)
            dataOutPtrs.resize(hardwareChannels);

        int32_t framesProcessed = 0;
        while (framesProcessed < numFrames) {
            const int32_t framesThisBlock = std::min(capacity, numFrames - framesProcessed);
            data.frameCount(framesThisBlock);

            zeroOutputBuses(framesThisBlock);

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

            mixToInterleaved(out + static_cast<size_t>(framesProcessed) * hardwareChannels,
                             framesThisBlock,
                             hardwareChannels);

            framesProcessed += framesThisBlock;
        }

        return DataCallbackResult::Continue;
    }

    void OboeAudioIODevice::zeroOutputBuses(int32_t frames) {
        if (data.audioOutBusCount() == 0)
            return;
        const uint32_t outChannels = static_cast<uint32_t>(data.outputChannelCount(0));
        for (uint32_t ch = 0; ch < outChannels; ++ch) {
            if (float* dst = data.getFloatOutBuffer(0, ch))
                std::fill(dst, dst + frames, 0.0f);
        }
    }

    void OboeAudioIODevice::mixToInterleaved(float* dst, int32_t frames, size_t hardwareChannels) {
        for (int32_t frame = 0; frame < frames; ++frame) {
            for (size_t ch = 0; ch < hardwareChannels; ++ch) {
                float sample = 0.0f;
                if (auto* src = dataOutPtrs[ch])
                    sample = src[frame];
                dst[frame * hardwareChannels + ch] = sample;
            }
        }
    }

    bool OboeAudioIODevice::renderEngineBlock(int32_t frames, size_t hardwareChannels) {
        data.frameCount(frames);
        zeroOutputBuses(frames);

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
                        for (int32_t frame = 0; frame < frames; ++frame)
                            mixDown[frame] += extra[frame];
                    }
                }
            }
        }

        if (dataOutPtrs.size() < hardwareChannels)
            dataOutPtrs.resize(hardwareChannels);
        for (size_t ch = 0; ch < hardwareChannels; ++ch) {
            dataOutPtrs[ch] = (ch < mappedChannels)
                ? data.getFloatOutBuffer(0, static_cast<uint32_t>(ch))
                : nullptr;
        }

        const size_t samples = static_cast<size_t>(frames) * hardwareChannels;
        stabilized_render_scratch_.resize(samples);
        mixToInterleaved(stabilized_render_scratch_.data(), frames, hardwareChannels);
        appendStabilizedBlock(stabilized_render_scratch_.data(), static_cast<size_t>(frames), hardwareChannels);
        return true;
    }

    void OboeAudioIODevice::appendStabilizedBlock(const float* interleaved, size_t frames, size_t hardwareChannels) {
        const size_t samples = frames * hardwareChannels;
        const size_t existingSamples = stabilized_buffered_frames_ * hardwareChannels;
        stabilized_buffer_.resize(existingSamples + samples);
        std::memcpy(stabilized_buffer_.data() + existingSamples, interleaved, samples * sizeof(float));
        stabilized_buffered_frames_ += frames;
    }

    void OboeAudioIODevice::consumeStabilizedFrames(float* dst, size_t frames, size_t hardwareChannels) {
        const size_t samples = frames * hardwareChannels;
        if (stabilized_buffer_.size() < samples)
            return;
        std::memcpy(dst, stabilized_buffer_.data(), samples * sizeof(float));
        const size_t remainingSamples = stabilized_buffer_.size() - samples;
        if (remainingSamples > 0)
            std::memmove(stabilized_buffer_.data(), stabilized_buffer_.data() + samples, remainingSamples * sizeof(float));
        stabilized_buffer_.resize(remainingSamples);
        stabilized_buffered_frames_ -= frames;
    }

    // Called on start() to make sure the FIFO already contains at least one full
    // host-sized render block before the HAL begins draining it. Prevents the first
    // AAudio callback from running short when preferred_callback_frames_ > numFrames.
    void OboeAudioIODevice::primeStabilizedBuffer() {
        if (!needsStabilizedMode() || callbacks.empty())
            return;
        if (output_channels_ == 0)
            return;
        const uint32_t targetFrames = std::max<uint32_t>(stabilized_block_frames_, preferred_callback_frames_);
        if (targetFrames == 0)
            return;
        size_t safetyCounter = 0;
        constexpr size_t kMaxPrimeIterations = 4;
        while (stabilized_buffered_frames_ < targetFrames && safetyCounter++ < kMaxPrimeIterations) {
            if (!renderEngineBlock(static_cast<int32_t>(targetFrames), static_cast<size_t>(output_channels_)))
                break;
        }
    }

    DataCallbackResult OboeAudioIODevice::processStabilized(AudioStream* audioStream,
                                                            float* audioData,
                                                            int32_t numFrames) {
        const size_t hardwareChannels = static_cast<size_t>(audioStream->getChannelCount());
        if (hardwareChannels == 0)
            return DataCallbackResult::Continue;
        const uint32_t renderFrames = std::max<uint32_t>(
            preferred_callback_frames_, static_cast<uint32_t>(numFrames));
        if (renderFrames == 0)
            return processImmediate(audioStream, audioData, numFrames);

        size_t safetyCounter = 0;
        constexpr size_t kMaxRenderAttempts = 8;
        while (stabilized_buffered_frames_ < static_cast<size_t>(numFrames)) {
            if (!renderEngineBlock(static_cast<int32_t>(renderFrames), hardwareChannels))
                return DataCallbackResult::Stop;
            if (++safetyCounter >= kMaxRenderAttempts)
                break;
        }

        if (stabilized_buffered_frames_ < static_cast<size_t>(numFrames)) {
            logger_->logWarning("OboeAudioIODevice: stabilized buffer underrun (need=%d have=%zu)",
                                numFrames, stabilized_buffered_frames_);
            return DataCallbackResult::Stop;
        }

        consumeStabilizedFrames(audioData, static_cast<size_t>(numFrames), hardwareChannels);
        return DataCallbackResult::Continue;
    }

    bool OboeAudioIODevice::onError(AudioStream *audioStream, Result error) {
        const auto xruns = audioStream ? audioStream->getXRunCount() : ResultWithValue<int32_t>{-1};
        logger_->logError("OboeAudioIODevice: stream error %s (xrunCount=%d)",
                          convertToText(error), xruns);
        playing_ = false;
        // Let Oboe perform the stop/close work on its own thread. We will optionally
        // reopen from onErrorAfterClose(), outside the before-close callback stage.
        return false;
    }

    void OboeAudioIODevice::onErrorAfterClose(AudioStream* audioStream, Result error) {
        (void) audioStream;
        if (!should_restart_after_error_.load())
            return;
        logger_->logWarning("OboeAudioIODevice: reopening stream after error %s", convertToText(error));
        if (!openStream())
            return;
        const auto startResult = stream_->requestStart();
        if (startResult == Result::OK) {
            playing_ = true;
            logger_->logInfo("OboeAudioIODevice: stream restarted after close");
            return;
        }
        logger_->logError("OboeAudioIODevice: restart after close failed: %s", convertToText(startResult));
        closeStream();
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
