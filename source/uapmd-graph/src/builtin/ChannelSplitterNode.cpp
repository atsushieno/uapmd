#include "uapmd-graph/uapmd-graph.hpp"
#include "uapmd-graph/detail/builtin/ChannelSplitterNode.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace uapmd::builtin {

    namespace {

        constexpr uint32_t kDefaultChannelCount = 6;
        constexpr uint32_t kMinChannelCount = 1;
        constexpr uint32_t kMaxChannelCount = 32;

        uint32_t getDescriptorChannelCount(
            const std::unordered_map<std::string, AudioGraphScalarValue>& values,
            const std::string& key,
            uint32_t fallback
        ) {
            auto it = values.find(key);
            if (it != values.end()) {
                double raw;
                if (auto value = std::get_if<int64_t>(&it->second))
                    raw = static_cast<double>(*value);
                else if (auto value = std::get_if<double>(&it->second))
                    raw = *value;
                else
                    return fallback;
                return static_cast<uint32_t>(std::clamp(raw, static_cast<double>(kMinChannelCount), static_cast<double>(kMaxChannelCount)));
            }
            return fallback;
        }

        void copyEvents(EventSequence& dst, EventSequence& src) {
            dst.position(0);
            if (src.position() == 0)
                return;
            const auto size = std::min(src.position(), dst.maxMessagesInBytes());
            std::memcpy(dst.getMessages(), src.getMessages(), size);
            dst.position(size);
        }

        class ChannelSplitterNodeImpl final : public ChannelSplitterNode {
            std::string node_id_;
            std::string display_name_;
            std::atomic<bool> bypassed_{false};
            uint32_t number_of_outputs_;
            ParameterUpdateEvent parameter_update_event_{};
            ParameterMetadataRefreshEvent parameter_metadata_refresh_event_{};

            // Mirrors ChannelMergerNode's dual shape: in the DAG graph this node owns a
            // private context with numberOfOutputs separate mono output buses (see
            // requiredAudioOutputChannelCounts), so audioOutBusCount() > 1 and each input
            // channel maps to one output bus. In the linear graph there is only ever the
            // track's single shared main bus (audioOutBusCount() == 1) - nowhere to route
            // separate outputs to - so "split" degrades to a straight channel passthrough
            // clamped to numberOfOutputs, silencing anything beyond it.
            template <typename SampleType>
            void splitChannels(AudioProcessContext& process) {
                const auto frameCount = static_cast<size_t>(std::max(process.frameCount(), 0));
                const auto inputChannels = static_cast<uint32_t>(process.inputChannelCount(0));
                const auto outputBuses = static_cast<uint32_t>(process.audioOutBusCount());

                if (outputBuses > 1) {
                    const auto n = std::min(inputChannels, outputBuses);
                    for (uint32_t bus = 0; bus < n; ++bus) {
                        auto* src = std::is_same_v<SampleType, double>
                            ? reinterpret_cast<SampleType*>(process.getDoubleInBuffer(0, bus))
                            : reinterpret_cast<SampleType*>(process.getFloatInBuffer(0, bus));
                        auto* dst = std::is_same_v<SampleType, double>
                            ? reinterpret_cast<SampleType*>(process.getDoubleOutBuffer(static_cast<int32_t>(bus), 0))
                            : reinterpret_cast<SampleType*>(process.getFloatOutBuffer(static_cast<int32_t>(bus), 0));
                        if (!src || !dst)
                            continue;
                        std::memcpy(dst, src, frameCount * sizeof(SampleType));
                    }
                    return;
                }

                const auto outputChannels = static_cast<uint32_t>(process.outputChannelCount(0));
                const auto n = std::min({inputChannels, outputChannels, number_of_outputs_});
                for (uint32_t ch = 0; ch < n; ++ch) {
                    auto* src = std::is_same_v<SampleType, double>
                        ? reinterpret_cast<SampleType*>(process.getDoubleInBuffer(0, ch))
                        : reinterpret_cast<SampleType*>(process.getFloatInBuffer(0, ch));
                    auto* dst = std::is_same_v<SampleType, double>
                        ? reinterpret_cast<SampleType*>(process.getDoubleOutBuffer(0, ch))
                        : reinterpret_cast<SampleType*>(process.getFloatOutBuffer(0, ch));
                    if (!src || !dst)
                        continue;
                    std::memcpy(dst, src, frameCount * sizeof(SampleType));
                }
            }

        public:
            explicit ChannelSplitterNodeImpl(const AudioGraphNodeDescriptor& descriptor)
                : node_id_(descriptor.node_id)
                , display_name_(descriptor.display_name.empty() ? "Channel Splitter" : descriptor.display_name)
                , number_of_outputs_(getDescriptorChannelCount(descriptor.options, "number_of_outputs", kDefaultChannelCount)) {
            }

            const std::string& nodeId() const override {
                return node_id_;
            }

            const std::string& nodeType() const override {
                static const std::string type{std::string(kChannelSplitterNodeType)};
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

            uint32_t numberOfOutputs() const override {
                return number_of_outputs_;
            }

            std::vector<uint32_t> requiredAudioInputChannelCounts() const override {
                return { number_of_outputs_ };
            }

            std::vector<uint32_t> requiredAudioOutputChannelCounts() const override {
                return std::vector<uint32_t>(number_of_outputs_, 1u);
            }

            int32_t processAudio(AudioProcessContext& process) override {
                copyEvents(process.eventOut(), process.eventIn());

                if (bypassed())
                    return 0;

                if (process.masterContext().audioDataType() == remidy::AudioContentType::Float64)
                    splitChannels<double>(process);
                else
                    splitChannels<float>(process);
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
        };

        class ChannelSplitterNodeFactory final : public AudioGraphBuiltInNodeFactory {
        public:
            std::string_view nodeType() const override {
                return kChannelSplitterNodeType;
            }

            std::unique_ptr<AudioGraphNode> create(const AudioGraphNodeDescriptor& descriptor) const override {
                return std::make_unique<ChannelSplitterNodeImpl>(descriptor);
            }
        };

    }

    std::unique_ptr<AudioGraphBuiltInNodeFactory> createChannelSplitterNodeFactory() {
        return std::make_unique<ChannelSplitterNodeFactory>();
    }

}
