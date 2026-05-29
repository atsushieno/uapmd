#include "uapmd-graph/uapmd-graph.hpp"
#include "uapmd-graph/detail/builtin/GainNode.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace uapmd::builtin {

    namespace {

        double getDescriptorDouble(
            const std::unordered_map<std::string, AudioGraphScalarValue>& values,
            const std::string& key,
            double fallback
        ) {
            auto it = values.find(key);
            if (it == values.end())
                return fallback;
            if (auto value = std::get_if<double>(&it->second))
                return *value;
            if (auto value = std::get_if<int64_t>(&it->second))
                return static_cast<double>(*value);
            return fallback;
        }

        class GainNodeImpl final : public GainNode {
            std::string node_id_;
            std::string display_name_;
            std::atomic<bool> bypassed_{false};
            std::atomic<double> target_gain_{1.0};
            double current_gain_{1.0};
            ParameterUpdateEvent parameter_update_event_{};
            ParameterMetadataRefreshEvent parameter_metadata_refresh_event_{};

            static double clampGain(double value) {
                return std::clamp(value, 0.0, 8.0);
            }

            void copyEvents(EventSequence& dst, EventSequence& src) {
                dst.position(0);
                if (src.position() == 0)
                    return;
                const auto size = std::min(src.position(), dst.maxMessagesInBytes());
                std::memcpy(dst.getMessages(), src.getMessages(), size);
                dst.position(size);
            }

            template <typename SampleType>
            void applyGainToOutputs(AudioProcessContext& process, double gainStep, double gainStart) {
                const auto frameCount = static_cast<size_t>(std::max(process.frameCount(), 0));
                for (int32_t bus = 0; bus < process.audioOutBusCount(); ++bus) {
                    const auto channelCount = static_cast<uint32_t>(process.outputChannelCount(bus));
                    for (uint32_t ch = 0; ch < channelCount; ++ch) {
                        auto* buffer = std::is_same_v<SampleType, double>
                            ? reinterpret_cast<SampleType*>(process.getDoubleOutBuffer(bus, ch))
                            : reinterpret_cast<SampleType*>(process.getFloatOutBuffer(bus, ch));
                        if (!buffer)
                            continue;
                        double gain = gainStart;
                        for (size_t frame = 0; frame < frameCount; ++frame) {
                            buffer[frame] = static_cast<SampleType>(buffer[frame] * gain);
                            gain += gainStep;
                        }
                    }
                }
            }

        public:
            explicit GainNodeImpl(const AudioGraphNodeDescriptor& descriptor)
                : node_id_(descriptor.node_id)
                , display_name_(descriptor.display_name.empty() ? "Gain" : descriptor.display_name) {
                current_gain_ = clampGain(getDescriptorDouble(descriptor.parameters, "gain", 1.0));
                target_gain_.store(current_gain_, std::memory_order_release);
            }

            const std::string& nodeId() const override {
                return node_id_;
            }

            const std::string& nodeType() const override {
                static const std::string type{std::string(kGainNodeType)};
                return type;
            }

            const std::string& displayName() const override {
                return display_name_;
            }

            bool bypassed() const override {
                return bypassed_.load(std::memory_order_acquire);
            }

            void bypassed(bool value) override {
                bypassed_.store(value, std::memory_order_release);
            }

            int32_t processAudio(AudioProcessContext& process) override {
                process.copyInputsToOutputs();
                copyEvents(process.eventOut(), process.eventIn());

                if (bypassed())
                    return 0;

                const auto targetGain = target_gain_.load(std::memory_order_acquire);
                const auto frameCount = static_cast<size_t>(std::max(process.frameCount(), 0));
                const auto gainStep = frameCount > 0 ? (targetGain - current_gain_) / static_cast<double>(frameCount) : 0.0;
                const auto gainStart = current_gain_;

                if (process.masterContext().audioDataType() == remidy::AudioContentType::Float64)
                    applyGainToOutputs<double>(process, gainStep, gainStart);
                else
                    applyGainToOutputs<float>(process, gainStep, gainStart);

                current_gain_ = targetGain;
                return 0;
            }

            uint32_t latencyInSamples() const override {
                return 0;
            }

            double tailLengthInSeconds() const override {
                return 0.0;
            }

            remidy::PluginAudioBuses* audioBuses() override {
                return nullptr;
            }

            ParameterUpdateEvent& parameterUpdateEvent() override {
                return parameter_update_event_;
            }

            ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() override {
                return parameter_metadata_refresh_event_;
            }

            double gain() const override {
                return target_gain_.load(std::memory_order_acquire);
            }

            void gain(double value) override {
                const auto clamped = clampGain(value);
                target_gain_.store(clamped, std::memory_order_release);
                parameter_update_event_.notify(0, clamped);
            }
        };

        class GainNodeFactory final : public AudioGraphBuiltInNodeFactory {
        public:
            std::string_view nodeType() const override {
                return kGainNodeType;
            }

            std::unique_ptr<AudioGraphNode> create(const AudioGraphNodeDescriptor& descriptor) const override {
                return std::make_unique<GainNodeImpl>(descriptor);
            }
        };

    }

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createGainNodeFactory() {
        return std::make_unique<GainNodeFactory>();
    }

}
