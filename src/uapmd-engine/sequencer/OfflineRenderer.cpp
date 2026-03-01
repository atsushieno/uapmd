#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_SampleBuffers.h>
#include <remidy/remidy.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd {
namespace {

double makePositiveSeconds(double value) {
    if (!std::isfinite(value))
        return 0.0;
    return std::max(0.0, value);
}

class EngineStateGuard {
public:
    explicit EngineStateGuard(SequencerEngine& engine)
        : engine_(engine),
          timelineState_(&engine.timeline().state()),
          previousTimelineState_(*timelineState_),
          previousPlaybackPosition_(engine.playbackPosition()),
          previousOffline_(engine.offlineRendering()),
          playbackWasActive_(engine.isPlaybackActive()) {}

    TimelineState& timelineState() { return *timelineState_; }

    ~EngineStateGuard() {
        engine_.pausePlayback();
        engine_.playbackPosition(previousPlaybackPosition_);
        if (timelineState_)
            *timelineState_ = previousTimelineState_;
        engine_.offlineRendering(previousOffline_);
        if (playbackWasActive_)
            engine_.resumePlayback();
        else
            engine_.pausePlayback();
    }

private:
    SequencerEngine& engine_;
    TimelineState* timelineState_{nullptr};
    TimelineState previousTimelineState_{};
    int64_t previousPlaybackPosition_{0};
    bool previousOffline_{false};
    bool playbackWasActive_{false};
};

} // namespace

OfflineRenderResult renderOfflineProject(SequencerEngine& engine,
                                         const OfflineRenderSettings& settings,
                                         const OfflineRenderCallbacks& callbacks) {
    OfflineRenderResult result;

    if (settings.outputPath.empty()) {
        result.errorMessage = "Output path is empty.";
        return result;
    }
    if (settings.sampleRate <= 0) {
        result.errorMessage = "Sample rate must be positive.";
        return result;
    }
    if (settings.bufferSize == 0) {
        result.errorMessage = "Buffer size must be greater than zero.";
        return result;
    }
    if (settings.outputChannels == 0) {
        result.errorMessage = "Output channels must be greater than zero.";
        return result;
    }
    if (settings.umpBufferSize == 0) {
        result.errorMessage = "UMP buffer size must be greater than zero.";
        return result;
    }

    const double startSeconds = makePositiveSeconds(settings.startSeconds);
    const int64_t startSample = static_cast<int64_t>(std::llround(startSeconds * static_cast<double>(settings.sampleRate)));

    std::optional<int64_t> explicitEndSample;
    if (settings.endSeconds.has_value()) {
        double endSeconds = makePositiveSeconds(*settings.endSeconds);
        if (endSeconds < startSeconds)
            endSeconds = startSeconds;
        explicitEndSample = static_cast<int64_t>(std::llround(endSeconds * static_cast<double>(settings.sampleRate)));
    }

    int64_t contentEndSample = startSample;
    if (explicitEndSample) {
        contentEndSample = *explicitEndSample;
    } else if (settings.useContentFallback && settings.contentBoundsValid) {
        double fallbackSeconds = std::max(makePositiveSeconds(settings.contentEndSeconds), startSeconds);
        contentEndSample = static_cast<int64_t>(std::llround(fallbackSeconds * static_cast<double>(settings.sampleRate)));
    } else {
        result.errorMessage = "Unable to determine render length.";
        return result;
    }

    if (contentEndSample <= startSample) {
        result.errorMessage = "Render range must be greater than zero.";
        return result;
    }

    const int64_t guardFrames = settings.tailSeconds > 0.0
        ? static_cast<int64_t>(std::llround(settings.tailSeconds * static_cast<double>(settings.sampleRate)))
        : 0;
    const int64_t hardStopSample = contentEndSample + guardFrames;
    const int64_t totalRenderFrames = std::max<int64_t>(1, hardStopSample - startSample);
    const double totalRenderSeconds = static_cast<double>(totalRenderFrames) / static_cast<double>(settings.sampleRate);

    const bool silenceStopEnabled = settings.enableSilenceStop && settings.silenceDurationSeconds > 0.0;
    const int64_t silenceStartSample = std::max(contentEndSample, startSample);
    const int64_t silenceHoldFrames = silenceStopEnabled
        ? static_cast<int64_t>(std::llround(settings.silenceDurationSeconds * static_cast<double>(settings.sampleRate)))
        : 0;
    const float silenceThreshold = silenceStopEnabled
        ? static_cast<float>(std::pow(10.0, settings.silenceThresholdDb / 20.0))
        : 0.0f;

    if (!settings.outputPath.parent_path().empty()) {
        std::error_code dirEc;
        std::filesystem::create_directories(settings.outputPath.parent_path(), dirEc);
        if (dirEc) {
            result.errorMessage = std::format("Cannot create output directory: {}", dirEc.message());
            return result;
        }
    }

    try {
        EngineStateGuard engineState(engine);
        engine.pausePlayback();
        engine.offlineRendering(true);

        auto& timelineState = engineState.timelineState();
        timelineState.isPlaying = true;
        timelineState.loopEnabled = false;
        timelineState.sample_rate = settings.sampleRate;
        timelineState.playheadPosition = TimelinePosition::fromSamples(
            startSample,
            settings.sampleRate,
            timelineState.tempo);
        timelineState.loopStart = timelineState.playheadPosition;
        timelineState.loopEnd = timelineState.playheadPosition;
        engine.playbackPosition(startSample);
        engine.resumePlayback();

        remidy::MasterContext masterContext;
        masterContext.sampleRate(settings.sampleRate);
        masterContext.isPlaying(true);
        masterContext.playbackPositionSamples(startSample);

        remidy::AudioProcessContext deviceContext(masterContext, settings.umpBufferSize);
        deviceContext.configureMainBus(settings.outputChannels, settings.outputChannels, settings.bufferSize);

        choc::audio::AudioFileProperties props;
        props.sampleRate = static_cast<double>(settings.sampleRate);
        props.numChannels = settings.outputChannels;
        props.bitDepth = choc::audio::BitDepth::float32;

        auto writer = choc::audio::WAVAudioFileFormat<true>().createWriter(settings.outputPath.string(), props);
        if (!writer) {
            result.errorMessage = "Failed to open output file for writing.";
            std::error_code removeEc;
            std::filesystem::remove(settings.outputPath, removeEc);
            return result;
        }

        int64_t currentSample = startSample;
        int64_t silenceFramesAccumulated = 0;
        std::vector<const float*> channelPtrs(settings.outputChannels, nullptr);
        OfflineRenderProgress progress{};

        while (currentSample < hardStopSample) {
            if (callbacks.shouldCancel && callbacks.shouldCancel()) {
                result.canceled = true;
                break;
            }

            const int64_t framesRemaining = hardStopSample - currentSample;
            const uint32_t framesToRender = static_cast<uint32_t>(std::min<int64_t>(framesRemaining, settings.bufferSize));
            if (framesToRender == 0)
                break;

            masterContext.playbackPositionSamples(currentSample);
            deviceContext.frameCount(framesToRender);

            for (uint32_t ch = 0; ch < settings.outputChannels; ++ch) {
                float* out = deviceContext.getFloatOutBuffer(0, ch);
                if (out)
                    std::memset(out, 0, framesToRender * sizeof(float));
                float* in = deviceContext.getFloatInBuffer(0, ch);
                if (in)
                    std::memset(in, 0, framesToRender * sizeof(float));
            }

            engine.processAudio(deviceContext);

            float peak = 0.0f;
            for (uint32_t ch = 0; ch < settings.outputChannels; ++ch) {
                const float* buffer = deviceContext.getFloatOutBuffer(0, ch);
                channelPtrs[ch] = buffer;
                if (!buffer)
                    continue;
                for (uint32_t frame = 0; frame < framesToRender; ++frame)
                    peak = std::max(peak, std::fabs(buffer[frame]));
            }

            if (silenceStopEnabled) {
                if (peak > silenceThreshold)
                    silenceFramesAccumulated = 0;
                else
                    silenceFramesAccumulated += framesToRender;
            }

            auto view = choc::buffer::createChannelArrayView(channelPtrs.data(), settings.outputChannels, framesToRender);
            writer->appendFrames(view);

            currentSample += framesToRender;

            progress.renderedFrames = currentSample - startSample;
            progress.totalFrames = totalRenderFrames;
            progress.renderedSeconds = static_cast<double>(progress.renderedFrames) / static_cast<double>(settings.sampleRate);
            progress.totalSeconds = totalRenderSeconds;
            progress.progress = std::clamp(
                static_cast<double>(progress.renderedFrames) / static_cast<double>(totalRenderFrames),
                0.0,
                1.0);

            if (callbacks.onProgress)
                callbacks.onProgress(progress);

            if (settings.enableSilenceStop &&
                currentSample >= silenceStartSample &&
                silenceFramesAccumulated >= silenceHoldFrames &&
                silenceHoldFrames > 0) {
                break;
            }
        }

        writer->flush();

        if (result.canceled) {
            std::error_code removeEc;
            std::filesystem::remove(settings.outputPath, removeEc);
            result.errorMessage = "Render canceled.";
            return result;
        }

        result.success = true;
        result.renderedSeconds = static_cast<double>(std::max<int64_t>(0, currentSample - startSample)) /
                                 static_cast<double>(settings.sampleRate);
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = e.what();
        std::error_code removeEc;
        std::filesystem::remove(settings.outputPath, removeEc);
        return result;
    }
}

} // namespace uapmd
