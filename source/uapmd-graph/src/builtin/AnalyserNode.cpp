#include "uapmd-graph/uapmd-graph.hpp"
#include "uapmd-graph/detail/builtin/AnalyserNode.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

namespace uapmd::builtin {

    namespace {

        constexpr uint32_t kFftSize = 256;
        constexpr uint32_t kFrequencyBinCount = kFftSize / 2;
        constexpr float kMinimumDecibels = -100.0f;
        constexpr float kSmoothingDecay = 0.85f;
        constexpr float kPi = 3.14159265358979323846f;

        void copyEvents(EventSequence& dst, EventSequence& src) {
            dst.position(0);
            const auto size = std::min(src.position(), dst.maxMessagesInBytes());
            if (size)
                std::memcpy(dst.getMessages(), src.getMessages(), size);
            dst.position(size);
        }

        class AnalyserNodeImpl final : public AnalyserNode {
            std::string node_id_;
            std::string display_name_;
            std::atomic<bool> bypassed_{false};
            std::array<float, kFftSize> time_history_{};
            std::array<std::atomic<float>, kFftSize> published_time_history_{};
            std::atomic<uint32_t> published_write_position_{0};
            std::array<std::atomic<float>, kFrequencyBinCount> magnitudes_{};
            ParameterUpdateEvent parameter_update_event_{};
            ParameterMetadataRefreshEvent parameter_metadata_refresh_event_{};
            uint32_t write_position_{0}; // audio thread only

            template<typename SampleType>
            void analyse(AudioProcessContext& process, bool input) {
                const auto busCount = input ? process.audioInBusCount() : process.audioOutBusCount();
                const auto channelCount = busCount > 0
                    ? static_cast<uint32_t>(input ? process.inputChannelCount(0) : process.outputChannelCount(0))
                    : 0u;
                if (process.frameCount() <= 0 || channelCount == 0)
                    return;

                for (uint32_t frame = 0; frame < static_cast<uint32_t>(process.frameCount()); ++frame) {
                    float sample = 0.0f;
                    for (uint32_t channel = 0; channel < channelCount; ++channel) {
                        const auto* buffer = input
                            ? (std::is_same_v<SampleType, double>
                                   ? reinterpret_cast<const SampleType*>(process.getDoubleInBuffer(0, channel))
                                   : reinterpret_cast<const SampleType*>(process.getFloatInBuffer(0, channel)))
                            : (std::is_same_v<SampleType, double>
                                   ? reinterpret_cast<const SampleType*>(process.getDoubleOutBuffer(0, channel))
                                   : reinterpret_cast<const SampleType*>(process.getFloatOutBuffer(0, channel)));
                        if (buffer)
                            sample += static_cast<float>(buffer[frame]);
                    }
                    sample /= static_cast<float>(channelCount);
                    time_history_[write_position_] = sample;
                    published_time_history_[write_position_].store(sample, std::memory_order_relaxed);
                    write_position_ = (write_position_ + 1) % kFftSize;
                }
                published_write_position_.store(write_position_, std::memory_order_release);

                std::array<float, kFftSize> samples{};
                for (uint32_t frame = 0; frame < kFftSize; ++frame) {
                    const auto sample = time_history_[(write_position_ + frame) % kFftSize];
                    const auto window = 0.5f - 0.5f * std::cos(
                        2.0f * kPi * static_cast<float>(frame) / static_cast<float>(kFftSize - 1));
                    samples[frame] = sample * window;
                }

                for (uint32_t bin = 0; bin < kFrequencyBinCount; ++bin) {
                    const auto angle = 2.0f * kPi * static_cast<float>(bin) / static_cast<float>(kFftSize);
                    const auto stepReal = std::cos(angle);
                    const auto stepImaginary = std::sin(angle);
                    float phaseReal = 1.0f;
                    float phaseImaginary = 0.0f;
                    float real = 0.0f;
                    float imaginary = 0.0f;
                    for (float sample : samples) {
                        real += sample * phaseReal;
                        imaginary -= sample * phaseImaginary;
                        const auto nextReal = phaseReal * stepReal - phaseImaginary * stepImaginary;
                        phaseImaginary = phaseReal * stepImaginary + phaseImaginary * stepReal;
                        phaseReal = nextReal;
                    }
                    const auto magnitude = std::min(
                        1.0f,
                        2.0f * std::sqrt(real * real + imaginary * imaginary) / static_cast<float>(kFftSize));
                    const auto previous = magnitudes_[bin].load(std::memory_order_relaxed);
                    magnitudes_[bin].store(
                        std::max(magnitude, previous * kSmoothingDecay),
                        std::memory_order_relaxed);
                }
            }

            void copyFrequencyData(float* values, uint32_t valueCount, bool decibels) const {
                if (!values)
                    return;
                for (uint32_t i = 0; i < valueCount; ++i) {
                    const auto bin = std::min(
                        kFrequencyBinCount - 1,
                        i * kFrequencyBinCount / std::max(valueCount, 1u));
                    const auto magnitude = magnitudes_[bin].load(std::memory_order_relaxed);
                    values[i] = decibels
                        ? std::max(kMinimumDecibels, 20.0f * std::log10(std::max(magnitude, 1.0e-5f)))
                        : magnitude;
                }
            }

        public:
            explicit AnalyserNodeImpl(const AudioGraphNodeDescriptor& descriptor)
                : node_id_(descriptor.node_id)
                , display_name_(descriptor.display_name.empty() ? "Analyser" : descriptor.display_name) {
                reset();
            }

            const std::string& nodeId() const override { return node_id_; }
            const std::string& nodeType() const override {
                static const std::string type{std::string(kAnalyserNodeType)};
                return type;
            }
            const std::string& displayName() const override { return display_name_; }
            bool bypassed() const override { return bypassed_.load(std::memory_order_acquire); }
            void bypassed(bool value) override { bypassed_.store(value, std::memory_order_release); }

            int32_t processAudio(AudioProcessContext& process) override {
                process.copyInputsToOutputs();
                copyEvents(process.eventOut(), process.eventIn());
                if (process.masterContext().audioDataType() == remidy::AudioContentType::Float64)
                    analyse<double>(process, false);
                else
                    analyse<float>(process, false);
                return 0;
            }
            uint32_t latencyInSamples() const override { return 0; }
            double tailLengthInSeconds() const override { return 0.0; }
            remidy::PluginAudioBuses* audioBuses() override { return nullptr; }
            ParameterUpdateEvent& parameterUpdateEvent() override { return parameter_update_event_; }
            ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() override { return parameter_metadata_refresh_event_; }

            uint32_t frequencyBinCount() const override { return kFrequencyBinCount; }
            void getFloatFrequencyData(float* values, uint32_t valueCount) const override { copyFrequencyData(values, valueCount, true); }
            void getFloatTimeDomainData(float* values, uint32_t valueCount) const override {
                if (!values)
                    return;
                const auto writePosition = published_write_position_.load(std::memory_order_acquire);
                for (uint32_t i = 0; i < valueCount; ++i) {
                    const auto sourceOffset = i * kFftSize / std::max(valueCount, 1u);
                    const auto sourceIndex = (writePosition + sourceOffset) % kFftSize;
                    values[i] = published_time_history_[sourceIndex].load(std::memory_order_relaxed);
                }
            }
            void reset() override {
                time_history_.fill(0.0f);
                for (auto& sample : published_time_history_)
                    sample.store(0.0f, std::memory_order_relaxed);
                for (auto& magnitude : magnitudes_)
                    magnitude.store(0.0f, std::memory_order_relaxed);
                write_position_ = 0;
                published_write_position_.store(0, std::memory_order_release);
            }
        };

        class AnalyserNodeFactory final : public AudioGraphBuiltInNodeFactory {
        public:
            std::string_view nodeType() const override { return kAnalyserNodeType; }
            std::unique_ptr<AudioGraphNode> create(const AudioGraphNodeDescriptor& descriptor) const override {
                return std::make_unique<AnalyserNodeImpl>(descriptor);
            }
        };
    }

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createAnalyserNodeFactory() {
        return std::make_unique<AnalyserNodeFactory>();
    }

    std::unique_ptr<AnalyserNode> createAnalyserNode(const AudioGraphNodeDescriptor& descriptor) {
        return std::make_unique<AnalyserNodeImpl>(descriptor);
    }

}
