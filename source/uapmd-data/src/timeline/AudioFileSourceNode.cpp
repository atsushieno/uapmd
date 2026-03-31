#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <signalsmith-stretch/signalsmith-stretch.h>

#include "uapmd-data/uapmd-data.hpp"

namespace uapmd {

    namespace {
        constexpr double kSampleRateTolerance = 0.01;
        constexpr double kMinimumRatio = 1.0e-9;

        struct WarpSegment {
            int64_t inputStart{0};
            int64_t inputSamples{0};
            int64_t outputSamples{0};
            double speedRatio{1.0};
        };

        std::vector<std::vector<float>> loadAndResampleToTarget(
            AudioFileReader& reader,
            double targetSampleRate,
            uint32_t& channelCount,
            int64_t& sourceFrames,
            double& sourceSampleRate
        ) {
            const auto& props = reader.getProperties();
            channelCount = props.numChannels;
            sourceFrames = props.numFrames;
            sourceSampleRate = props.sampleRate;

            std::vector<std::vector<float>> sourceBuffer(channelCount);
            for (uint32_t ch = 0; ch < channelCount; ++ch)
                sourceBuffer[ch].resize(sourceFrames);

            std::vector<float*> destPtrs;
            destPtrs.reserve(channelCount);
            for (uint32_t ch = 0; ch < channelCount; ++ch)
                destPtrs.push_back(sourceBuffer[ch].data());
            reader.readFrames(0, sourceFrames, destPtrs.data(), channelCount);

            if (std::abs(sourceSampleRate - targetSampleRate) <= kSampleRateTolerance || sourceFrames <= 0)
                return sourceBuffer;

            const double ratio = targetSampleRate / sourceSampleRate;
            int64_t targetFrames = static_cast<int64_t>(std::llround(static_cast<double>(sourceFrames) * ratio));
            targetFrames = std::max<int64_t>(0, targetFrames);

            std::vector<std::vector<float>> resampled(channelCount);
            for (uint32_t ch = 0; ch < channelCount; ++ch)
                resampled[ch].resize(targetFrames);

            for (uint32_t ch = 0; ch < channelCount; ++ch) {
                for (int64_t i = 0; i < targetFrames; ++i) {
                    double sourcePos = static_cast<double>(i) / ratio;
                    int64_t sourceIndex = static_cast<int64_t>(sourcePos);
                    double frac = sourcePos - static_cast<double>(sourceIndex);
                    sourceIndex = std::clamp<int64_t>(sourceIndex, 0, std::max<int64_t>(0, sourceFrames - 1));
                    int64_t nextIndex = std::min<int64_t>(sourceIndex + 1, std::max<int64_t>(0, sourceFrames - 1));
                    float a = sourceBuffer[ch][sourceIndex];
                    float b = sourceBuffer[ch][nextIndex];
                    resampled[ch][i] = a + static_cast<float>(frac) * (b - a);
                }
            }

            sourceFrames = targetFrames;
            sourceSampleRate = targetSampleRate;
            return resampled;
        }

        std::vector<AudioWarpPoint> normalizeWarps(std::vector<AudioWarpPoint> audioWarps, int64_t sourceFrames) {
            std::stable_sort(audioWarps.begin(), audioWarps.end(), [](const AudioWarpPoint& a, const AudioWarpPoint& b) {
                return a.clipPositionOffset < b.clipPositionOffset;
            });

            std::vector<AudioWarpPoint> normalized;
            normalized.reserve(audioWarps.size() + 1);
            normalized.push_back(AudioWarpPoint{});
            normalized.back().clipPositionOffset = 0;
            normalized.back().speedRatio = 1.0;

            for (const auto& warp : audioWarps) {
                AudioWarpPoint value = warp;
                value.clipPositionOffset = std::max<int64_t>(0, value.clipPositionOffset);
                if (!std::isfinite(value.speedRatio) || value.speedRatio <= 0.0)
                    value.speedRatio = 1.0;

                if (value.clipPositionOffset == normalized.back().clipPositionOffset)
                    normalized.back() = std::move(value);
                else
                    normalized.push_back(std::move(value));
            }

            return normalized;
        }

        std::vector<WarpSegment> buildWarpSegments(const std::vector<AudioWarpPoint>& warps, int64_t sourceFrames) {
            std::vector<WarpSegment> segments;
            if (warps.empty())
                return segments;

            segments.reserve(warps.size());
            int64_t currentSourcePosition = 0;
            for (size_t i = 0; i + 1 < warps.size(); ++i) {
                const auto& start = warps[i];
                const auto& end = warps[i + 1];
                WarpSegment segment;
                currentSourcePosition = std::clamp<int64_t>(currentSourcePosition, 0, sourceFrames);
                segment.inputStart = currentSourcePosition;
                const int64_t outputSamples = std::max<int64_t>(0, end.clipPositionOffset - start.clipPositionOffset);
                const int64_t nextSourcePosition = std::clamp<int64_t>(
                    currentSourcePosition + static_cast<int64_t>(std::llround(
                        static_cast<double>(outputSamples) * std::max(start.speedRatio, kMinimumRatio))),
                    0,
                    sourceFrames);
                segment.inputSamples = std::max<int64_t>(0, nextSourcePosition - currentSourcePosition);
                segment.outputSamples = outputSamples;
                segment.speedRatio = start.speedRatio;
                segments.push_back(segment);
                currentSourcePosition = nextSourcePosition;
            }

            const auto& last = warps.back();
            WarpSegment tail;
            tail.inputStart = currentSourcePosition;
            tail.inputSamples = std::max<int64_t>(0, sourceFrames - tail.inputStart);
            tail.speedRatio = std::max(last.speedRatio, kMinimumRatio);
            tail.outputSamples = static_cast<int64_t>(std::llround(static_cast<double>(tail.inputSamples) / tail.speedRatio));
            tail.outputSamples = std::max<int64_t>(0, tail.outputSamples);
            segments.push_back(tail);
            return segments;
        }

        void renderLinearFallback(
            const std::vector<std::vector<float>>& input,
            int64_t inputStart,
            int64_t inputSamples,
            std::vector<std::vector<float>>& output,
            int64_t outputOffset,
            int64_t outputSamples
        ) {
            if (outputSamples <= 0)
                return;

            const uint32_t channelCount = static_cast<uint32_t>(std::min(input.size(), output.size()));
            if (inputSamples <= 0) {
                for (uint32_t ch = 0; ch < channelCount; ++ch)
                    std::fill_n(output[ch].data() + outputOffset, outputSamples, 0.0f);
                return;
            }

            for (uint32_t ch = 0; ch < channelCount; ++ch) {
                auto* out = output[ch].data() + outputOffset;
                const auto* in = input[ch].data() + inputStart;
                if (inputSamples == 1) {
                    std::fill_n(out, outputSamples, in[0]);
                    continue;
                }
                for (int64_t i = 0; i < outputSamples; ++i) {
                    double t = outputSamples > 1
                        ? static_cast<double>(i) / static_cast<double>(outputSamples - 1)
                        : 0.0;
                    double sourcePos = t * static_cast<double>(inputSamples - 1);
                    int64_t index = static_cast<int64_t>(sourcePos);
                    double frac = sourcePos - static_cast<double>(index);
                    int64_t nextIndex = std::min<int64_t>(index + 1, inputSamples - 1);
                    float a = in[index];
                    float b = in[nextIndex];
                    out[i] = a + static_cast<float>(frac) * (b - a);
                }
            }
        }

        std::vector<std::vector<float>> renderWarpedBuffer(
            const std::vector<std::vector<float>>& source,
            double targetSampleRate,
            const std::vector<AudioWarpPoint>& audioWarps,
            int64_t& renderedFrames
        ) {
            renderedFrames = 0;
            const uint32_t channelCount = static_cast<uint32_t>(source.size());
            if (channelCount == 0)
                return {};

            const int64_t sourceFrames = static_cast<int64_t>(source[0].size());
            auto normalizedWarps = normalizeWarps(audioWarps, sourceFrames);
            auto segments = buildWarpSegments(normalizedWarps, sourceFrames);

            for (const auto& segment : segments)
                renderedFrames += segment.outputSamples;

            std::vector<std::vector<float>> rendered(channelCount);
            for (uint32_t ch = 0; ch < channelCount; ++ch)
                rendered[ch].assign(renderedFrames, 0.0f);

            signalsmith::stretch::SignalsmithStretch<float> stretch;
            stretch.presetDefault(static_cast<int>(channelCount), static_cast<float>(targetSampleRate), true);

            int64_t outputOffset = 0;
            for (const auto& segment : segments) {
                if (segment.outputSamples <= 0)
                    continue;
                if (segment.inputSamples <= 0) {
                    outputOffset += segment.outputSamples;
                    continue;
                }

                std::vector<float*> outputPtrs;
                outputPtrs.reserve(channelCount);
                for (uint32_t ch = 0; ch < channelCount; ++ch)
                    outputPtrs.push_back(rendered[ch].data() + outputOffset);

                bool renderedWithStretch = false;
                if (segment.inputSamples >= stretch.outputSeekLength(static_cast<float>(segment.speedRatio))) {
                    stretch.reset();
                    std::vector<const float*> inputPtrs;
                    inputPtrs.reserve(channelCount);
                    for (uint32_t ch = 0; ch < channelCount; ++ch)
                        inputPtrs.push_back(source[ch].data() + segment.inputStart);
                    renderedWithStretch = stretch.exact(inputPtrs,
                                                        static_cast<int>(segment.inputSamples),
                                                        outputPtrs,
                                                        static_cast<int>(segment.outputSamples));
                }

                if (!renderedWithStretch) {
                    renderLinearFallback(source,
                                         segment.inputStart,
                                         segment.inputSamples,
                                         rendered,
                                         outputOffset,
                                         segment.outputSamples);
                }

                outputOffset += segment.outputSamples;
            }

            return rendered;
        }
    } // namespace

    AudioFileSourceNode::AudioFileSourceNode(
        int32_t instanceId,
        std::unique_ptr<uapmd::AudioFileReader> reader,
        double targetSampleRate
    ) : AudioFileSourceNode(instanceId, std::move(reader), targetSampleRate, {}) {}

    AudioFileSourceNode::AudioFileSourceNode(
        int32_t instanceId,
        std::unique_ptr<uapmd::AudioFileReader> reader,
        double targetSampleRate,
        std::vector<AudioWarpPoint> audioWarps
    ) : instance_id_(instanceId),
        reader_(std::move(reader)),
        target_sample_rate_(targetSampleRate),
        audio_warps_(std::move(audioWarps)) {

        if (!reader_)
            return;

        audio_buffer_ = loadAndResampleToTarget(*reader_, targetSampleRate, channel_count_, num_frames_, sample_rate_);
        if (!audio_warps_.empty()) {
            int64_t warpedFrames = 0;
            auto warpedBuffer = renderWarpedBuffer(audio_buffer_, targetSampleRate, audio_warps_, warpedFrames);
            if (!warpedBuffer.empty()) {
                audio_buffer_ = std::move(warpedBuffer);
                num_frames_ = warpedFrames;
                sample_rate_ = targetSampleRate;
            }
        }

        // Mark buffer as ready for realtime reading (memory barrier ensures visibility)
        buffer_ready_.store(true, std::memory_order_release);
    }

    void AudioFileSourceNode::seek(int64_t samplePosition) {
        playback_position_.store(samplePosition, std::memory_order_release);
        // Update source position (convert from target rate to source rate)
        if (std::abs(sample_rate_ - target_sample_rate_) > kSampleRateTolerance) {
            source_position_.store(samplePosition * (sample_rate_ / target_sample_rate_), std::memory_order_release);
        } else {
            source_position_.store(samplePosition, std::memory_order_release);
        }
    }

    int64_t AudioFileSourceNode::currentPosition() const {
        return playback_position_.load(std::memory_order_acquire);
    }

    int64_t AudioFileSourceNode::totalLength() const {
        // Return length in target sample rate domain
        if (std::abs(sample_rate_ - target_sample_rate_) > kSampleRateTolerance) {
            double ratio = target_sample_rate_ / sample_rate_;
            return static_cast<int64_t>(std::round(num_frames_ * ratio));
        }
        return num_frames_;
    }

    void AudioFileSourceNode::processAudio(float** buffers, uint32_t numChannels, int32_t frameCount) {
        // Clear output buffers first
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            if (buffers[ch])
                std::memset(buffers[ch], 0, frameCount * sizeof(float));
        }

        if (bypassed_ || !is_playing_.load(std::memory_order_acquire))
            return;

        // Realtime-safe check: is buffer ready? (no blocking)
        if (!buffer_ready_.load(std::memory_order_acquire))
            return;

        if (audio_buffer_.empty())
            return;

        int64_t pos = playback_position_.load(std::memory_order_acquire);

        // Check if resampling is needed
        bool needsResampling = std::abs(sample_rate_ - target_sample_rate_) > kSampleRateTolerance;

        if (!needsResampling) {
            // Fast path: no resampling needed, direct copy
            if (pos < 0 || pos >= num_frames_)
                return;

            int64_t framesToCopy = std::min(static_cast<int64_t>(frameCount), num_frames_ - pos);
            if (framesToCopy <= 0)
                return;

            for (uint32_t ch = 0; ch < std::min(numChannels, channel_count_); ++ch) {
                if (buffers[ch] && ch < audio_buffer_.size()) {
                    const float* src = &audio_buffer_[ch][pos];
                    std::memcpy(buffers[ch], src, framesToCopy * sizeof(float));
                }
            }

            playback_position_.store(pos + framesToCopy, std::memory_order_release);
        } else {
            // Resampling path: use linear interpolation
            double resampleIncrement = sample_rate_ / target_sample_rate_;
            int32_t framesCopied = 0;

            // Load current source position atomically
            double currentSourcePos = source_position_.load(std::memory_order_acquire);

            for (int32_t i = 0; i < frameCount; ++i) {
                // Check if we're past the end of the source buffer
                if (currentSourcePos >= num_frames_ - 1)
                    break;

                int64_t sourceIndex = static_cast<int64_t>(currentSourcePos);
                double fraction = currentSourcePos - sourceIndex;

                // Linear interpolation between adjacent samples
                // Reading from pre-allocated buffer (realtime-safe)
                for (uint32_t ch = 0; ch < std::min(numChannels, channel_count_); ++ch) {
                    if (buffers[ch] && ch < audio_buffer_.size()) {
                        float sample0 = audio_buffer_[ch][sourceIndex];
                        float sample1 = audio_buffer_[ch][sourceIndex + 1];
                        buffers[ch][i] = sample0 + fraction * (sample1 - sample0);
                    }
                }

                currentSourcePos += resampleIncrement;
                framesCopied++;
            }

            // Store updated source position atomically
            source_position_.store(currentSourcePos, std::memory_order_release);
            playback_position_.store(pos + framesCopied, std::memory_order_release);
        }
    }

    std::vector<uint8_t> AudioFileSourceNode::saveState() {
        // Simple state: just the playback position
        std::vector<uint8_t> state(sizeof(int64_t));
        int64_t pos = playback_position_.load(std::memory_order_acquire);
        std::memcpy(state.data(), &pos, sizeof(int64_t));
        return state;
    }

    void AudioFileSourceNode::loadState(const std::vector<uint8_t>& state) {
        if (state.size() >= sizeof(int64_t)) {
            int64_t pos;
            std::memcpy(&pos, state.data(), sizeof(int64_t));
            playback_position_.store(pos, std::memory_order_release);
        }
    }

} // namespace uapmd
