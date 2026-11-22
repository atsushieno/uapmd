#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstring>
#include "uapmd-engine/uapmd-engine.hpp"

using namespace uapmd_graph;

namespace uapmd {
    SequencerTrack::SequencerTrack(size_t eventBufferSizeInBytes) : plugin_output_events_(eventBufferSizeInBytes / sizeof(uapmd_ump_t)) {}

    static_assert(std::atomic<float>::is_always_lock_free);

    namespace {
        std::optional<double> numericParameter(const choc::value::ValueView& value) {
            if (value.isInt() || value.isFloat()) {
                const auto number = value.get<double>();
                if (std::isfinite(number))
                    return number;
            }
            return std::nullopt;
        }
    }

    const TrackOutputPanners& SequencerTrack::outputPanners() const {
        return output_panners_.currentOnPublisherThread()->authored;
    }

    bool SequencerTrack::outputPanners(TrackOutputPanners value) {
        const auto compileMatrix = [](const choc::value::ValueView& matrix) -> std::optional<ChannelMatrixPanner> {
            if (!matrix.isArray() || matrix.size() < 3)
                return std::nullopt;
            ChannelMatrixPanner node;
            for (const auto& row : matrix) {
                if (!row.isArray() || row.size() != matrix.size())
                    return std::nullopt;
                auto& coefficients = node.rows.emplace_back();
                for (uint32_t input = 0; input < row.size(); ++input) {
                    const auto coefficient = numericParameter(row[input]);
                    if (!coefficient || std::abs(*coefficient) > std::numeric_limits<float>::max())
                        return std::nullopt;
                    const auto gain = static_cast<float>(*coefficient);
                    if (gain != 0.0f)
                        coefficients.push_back({input, gain});
                }
            }
            return node;
        };
        auto snapshot = std::make_unique<OutputPannerSnapshot>();
        try {
            for (const auto& [bus, panner] : value) {
                const auto parameters = choc::json::parse(panner.parameters_json);
                if (panner.node_type.empty() || !parameters.isObject())
                    return false;
                if (panner.node_type == kStereoBalancePannerType) {
                    const auto balance = numericParameter(parameters["balance"]);
                    if (balance && *balance >= -1.0 && *balance <= 1.0)
                        snapshot->nodes.emplace(bus, StereoBalancePanner{static_cast<float>(*balance)});
                } else if (panner.node_type == kChannelMatrixPannerType) {
                    if (auto node = compileMatrix(parameters["matrix"]))
                        snapshot->nodes.emplace(bus, std::move(*node));
                }
            }
        } catch (const choc::json::ParseError&) {
            return false;
        }
        snapshot->authored = std::move(value);
        output_panners_.publish(std::move(snapshot));
        return true;
    }

    std::optional<float> SequencerTrack::stereoBalance(uint32_t bus) const {
        if (outputMeterChannelCount(bus) != 2)
            return std::nullopt;
        const auto* snapshot = output_panners_.currentOnPublisherThread();
        const auto authored = snapshot->authored.find(bus);
        if (authored == snapshot->authored.end())
            return 0.0f;
        if (authored->second.node_type != kStereoBalancePannerType)
            return std::nullopt;
        const auto node = snapshot->nodes.find(bus);
        return node == snapshot->nodes.end() ? 0.0f : std::get<StereoBalancePanner>(node->second).balance;
    }

    TrackOutputPanners SequencerTrack::outputPannersWithStereoBalance(uint32_t bus, float balance) const {
        auto panners = outputPanners();
        if (!stereoBalance(bus) || !std::isfinite(balance) || balance < -1.0f || balance > 1.0f)
            return panners;
        const auto existing = panners.find(bus);
        auto parameters = existing == panners.end() ? choc::value::createObject("Parameters")
            : choc::json::parse(existing->second.parameters_json);
        parameters.setMember("balance", static_cast<double>(balance));
        if (balance == 0.0f && parameters.size() == 1)
            panners.erase(bus);
        else
            panners.insert_or_assign(bus, TrackOutputPanner{
                std::string(kStereoBalancePannerType), choc::json::toString(parameters)
            });
        return panners;
    }

    void SequencerTrack::applyChannelMatrix(
        const ChannelMatrixPanner& panner, AudioProcessContext& context, uint32_t bus, int32_t frames) {
        const auto channelCount = static_cast<uint32_t>(context.outputChannelCount(bus));
        if (panner.rows.size() != channelCount || channelCount > panner_channels_.size())
            return;
        for (uint32_t channel = 0; channel < channelCount; ++channel) {
            panner_channels_[channel] = context.getFloatOutBuffer(bus, channel);
            if (!panner_channels_[channel])
                return;
        }
        for (int32_t frame = 0; frame < frames; ++frame) {
            for (uint32_t input = 0; input < channelCount; ++input)
                panner_input_[input] = panner_channels_[input][frame];
            for (uint32_t output = 0; output < channelCount; ++output) {
                float sample = 0.0f;
                for (const auto& [input, gain] : panner.rows[output])
                    sample += gain * panner_input_[input];
                panner_channels_[output][frame] = sample;
            }
        }
    }

    float SequencerTrack::applyOutputPanners(AudioProcessContext& context, int32_t frames) {
        if (frames <= 0)
            return 0.0f;
        const auto panners = output_panners_.protect();
        const auto busCount = std::min(current_stereo_balances_.size(), static_cast<size_t>(context.audioOutBusCount()));
        float mainStereoBalance = 0.0f;
        for (uint32_t bus = 0; bus < busCount; ++bus) {
            const auto node = panners->nodes.find(bus);
            auto& previous = current_stereo_balances_[bus];
            if (context.outputChannelCount(bus) != 2) {
                previous = 0.0f;
                if (node != panners->nodes.end())
                    if (const auto* matrix = std::get_if<ChannelMatrixPanner>(&node->second))
                        applyChannelMatrix(*matrix, context, bus, frames);
                continue;
            }

            const auto* stereo = node == panners->nodes.end() ? nullptr : std::get_if<StereoBalancePanner>(&node->second);
            const float target = stereo ? stereo->balance : 0.0f;
            if (bus == 0)
                mainStereoBalance = target;
            if (target == 0.0f && previous == 0.0f)
                continue;
            for (uint32_t channel = 0; channel < 2; ++channel) {
                auto* samples = context.getFloatOutBuffer(bus, channel);
                if (!samples)
                    continue;
                const float side = channel == 0 ? 1.0f : -1.0f;
                const float start = 1.0f - std::max(0.0f, side * previous);
                const float end = 1.0f - std::max(0.0f, side * target);
                if (start == 1.0f && end == 1.0f)
                    continue;
                const float step = (end - start) / static_cast<float>(frames);
                for (int32_t frame = 0; frame < frames; ++frame)
                    samples[frame] *= start + step * static_cast<float>(frame);
            }
            previous = target;
        }
        return mainStereoBalance;
    }

    void SequencerTrack::configureOutputMixing(const AudioProcessContext& context) {
        const auto busCount = static_cast<size_t>(context.audioOutBusCount());
        output_peaks_.resize(busCount);
        current_stereo_balances_.resize(busCount);
        size_t maxChannels = 0;
        for (size_t bus = 0; bus < busCount; ++bus) {
            const auto channels = static_cast<size_t>(context.outputChannelCount(static_cast<int32_t>(bus)));
            if (output_peaks_[bus].size() != channels) {
                output_peaks_[bus] = std::vector<std::atomic<float>>(channels);
                current_stereo_balances_[bus] = 0.0f;
            }
            maxChannels = std::max(maxChannels, channels);
        }
        panner_input_.resize(maxChannels);
        panner_channels_.resize(maxChannels);
    }

    void SequencerTrack::captureOutputPeaks(const AudioProcessContext& context, int32_t frames) {
        const auto busCount = std::min(output_peaks_.size(), static_cast<size_t>(context.audioOutBusCount()));
        for (size_t bus = 0; bus < busCount; ++bus) {
            const auto busIndex = static_cast<int32_t>(bus);
            const auto channels = std::min(output_peaks_[bus].size(), static_cast<size_t>(context.outputChannelCount(busIndex)));
            for (size_t channel = 0; channel < channels; ++channel) {
                const auto* samples = context.getFloatOutBuffer(busIndex, static_cast<uint32_t>(channel));
                if (!samples)
                    continue;
                float peak = 0.0f;
                for (int32_t frame = 0; frame < frames; ++frame)
                    peak = std::max(peak, std::abs(samples[frame]));

                auto& pending = output_peaks_[bus][channel];
                float previous = pending.load(std::memory_order_relaxed);
                // The only competing writer is the UI, which exchanges the value for zero.
                if (peak > previous && !pending.compare_exchange_strong(previous, peak, std::memory_order_relaxed))
                    pending.store(peak, std::memory_order_relaxed);
            }
        }
    }

    float SequencerTrack::consumeOutputPeak(size_t bus, size_t channel) {
        if (channel >= outputMeterChannelCount(bus))
            return 0.0f;
        return output_peaks_[bus][channel].exchange(0.0f, std::memory_order_relaxed);
    }

    void SequencerTrack::capturePluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
        if (!data)
            return;
        const auto* source = reinterpret_cast<const uint8_t*>(data);
        size_t offset = 0;
        while (bytes - offset >= sizeof(uapmd_ump_t)) {
            uapmd_ump_t firstWord;
            std::memcpy(&firstWord, source + offset, sizeof(firstWord));
            const auto messageBytes = static_cast<size_t>(umppi::umpSizeInInts(firstWord >> 28)) * sizeof(uapmd_ump_t);
            if (messageBytes == 0 || messageBytes > 4 * sizeof(uapmd_ump_t) || messageBytes > bytes - offset)
                break;
            if (plugin_output_event_count_ < plugin_output_events_.size()) {
                auto& event = plugin_output_events_[plugin_output_event_count_++];
                event.preset_request = false;
                event.instance_id = instanceId;
                event.size_in_bytes = static_cast<uint32_t>(messageBytes);
                event.words.fill(0);
                std::memcpy(event.words.data(), source + offset, messageBytes);
            } else
                dropped_plugin_output_events_.fetch_add(1, std::memory_order_relaxed);
            offset += messageBytes;
        }
    }

    void SequencerTrack::capturePresetRequest(int32_t instanceId, uint32_t index) {
        if (plugin_output_event_count_ == plugin_output_events_.size()) {
            dropped_plugin_output_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& event = plugin_output_events_[plugin_output_event_count_++];
        event = {};
        event.instance_id = instanceId;
        event.preset_request = true;
        event.words[0] = index;
    }

    constexpr std::string_view kTrackGainNodeId = "builtin:track_gain";

    AudioGraphNodeDescriptor createTrackGainNodeDescriptor() {
        AudioGraphNodeDescriptor gainNode;
        gainNode.node_id = std::string(kTrackGainNodeId);
        gainNode.node_type = std::string(webaudio_compat::kGainNodeType);
        gainNode.display_name = "Track Volume";
        gainNode.parameters.emplace("gain", 1.0);
        return gainNode;
    }

    class SequencerTrackImpl : public SequencerTrack {
        bool bypass_{false};
        bool frozen_{false};
        std::atomic_bool muted_{false};
        std::atomic_bool solo_{false};
        std::unique_ptr<AudioPluginGraph> graph_;
        // Looking the node up takes the graph's non-realtime lock, so the audio thread reads it from here.
        std::atomic<webaudio_compat::GainNode*> track_gain_node_{nullptr};
        std::vector<int32_t> instance_ids{};
        std::unordered_map<int32_t, uint8_t> instance_groups_{}; // instanceId → UMP group

    public:
        explicit SequencerTrackImpl(std::unique_ptr<AudioPluginGraph>&& graph, size_t eventBufferSizeInBytes);
        ~SequencerTrackImpl() override = default;

        AudioPluginGraph& graph() override { return *graph_; }
        bool replaceGraph(std::unique_ptr<AudioPluginGraph>&& graph) override;
        uint32_t latencyInSamples() override { return graph_ ? graph_->mainOutputLatencyInSamples() : 0; }
        uint32_t renderLeadInSamples() override { return graph_ ? graph_->renderLeadInSamples() : 0; }
        double tailLengthInSeconds() override { return graph_ ? graph_->mainOutputTailLengthInSeconds() : 0.0; }
        double trackGain() const override;
        bool trackGain(double value) override;
        bool muted() const override { return muted_.load(std::memory_order_acquire); }
        void muted(bool value) override { muted_.store(value, std::memory_order_release); }
        bool solo() const override { return solo_.load(std::memory_order_acquire); }
        void solo(bool value) override { solo_.store(value, std::memory_order_release); }

        std::vector<int32_t>& orderedInstanceIds() override {
            return instance_ids;
        }

        bool bypassed() override { return bypass_; }
        bool frozen() override { return frozen_; }
        void bypassed(bool value) override { bypass_ = value; }
        void frozen(bool value) override { frozen_ = value; }

        void setInstanceGroup(int32_t instanceId, uint8_t group) override {
            instance_groups_[instanceId] = group;
        }

        uint8_t getInstanceGroup(int32_t instanceId) const override {
            auto it = instance_groups_.find(instanceId);
            return it != instance_groups_.end() ? it->second : 0xFFu;
        }

        uint8_t findAvailableGroup() const override {
            for (uint8_t g = 0; g < 16; ++g) {
                bool inUse = false;
                for (const auto& [id, grp] : instance_groups_)
                    if (grp == g) { inUse = true; break; }
                if (!inUse) return g;
            }
            return 0xFFu; // all 16 groups taken
        }

        void removeInstance(int32_t instanceId) override {
            std::erase(instance_ids, instanceId);
            instance_groups_.erase(instanceId);
        }

    private:
        webaudio_compat::GainNode* findTrackGainNode() const;
        webaudio_compat::GainNode* ensureTrackGainNode();
    };

    SequencerTrackImpl::SequencerTrackImpl(std::unique_ptr<AudioPluginGraph>&& graph, size_t eventBufferSizeInBytes) :
        SequencerTrack(eventBufferSizeInBytes),
        graph_(std::move(graph)) {
        track_gain_node_.store(findTrackGainNode(), std::memory_order_release);
    }

    std::unique_ptr<SequencerTrack> SequencerTrack::create(
        const AudioGraphProviderRegistry& registry,
        size_t eventBufferSizeInBytes,
        const std::string& graphProviderId) {
        auto graph = registry.createGraph(graphProviderId, eventBufferSizeInBytes);
        if (!graph) {
            // Only the default graph type may fall back. A caller that asked
            // for a named provider gets nothing rather than a lesser graph
            // silently standing in for the one it requested.
            if (!graphProviderId.empty())
                return nullptr;
            graph = AudioPluginGraph::create(eventBufferSizeInBytes);
            if (!graph)
                return nullptr;
        }
        graph->appendBuiltInNodeSimple(createTrackGainNodeDescriptor());
        return std::make_unique<SequencerTrackImpl>(std::move(graph), eventBufferSizeInBytes);
    }

    bool SequencerTrackImpl::replaceGraph(std::unique_ptr<AudioPluginGraph>&& graph) {
        if (!graph_ || !graph)
            return false;
        if (!AudioPluginGraph::migrate(*graph, *graph_))
            return false;
        track_gain_node_.store(nullptr, std::memory_order_release);
        graph_ = std::move(graph);
        ensureTrackGainNode();
        // Choosing a graph replaces whatever unloadable one this track was
        // standing in for, so there is nothing left to preserve.
        clearUnresolvedGraph();
        return true;
    }

    webaudio_compat::GainNode* SequencerTrackImpl::findTrackGainNode() const {
        if (!graph_)
            return nullptr;
        auto* node = graph_->getNode(std::string(kTrackGainNodeId));
        return dynamic_cast<webaudio_compat::GainNode*>(node);
    }

    webaudio_compat::GainNode* SequencerTrackImpl::ensureTrackGainNode() {
        auto* gain = findTrackGainNode();
        if (!gain && graph_ && graph_->appendBuiltInNodeSimple(createTrackGainNodeDescriptor()) == 0)
            gain = findTrackGainNode();
        track_gain_node_.store(gain, std::memory_order_release);
        return gain;
    }

    double SequencerTrackImpl::trackGain() const {
        auto* gain = track_gain_node_.load(std::memory_order_acquire);
        return gain ? gain->gain() : 1.0;
    }

    bool SequencerTrackImpl::trackGain(double value) {
        auto* gain = ensureTrackGainNode();
        if (!gain)
            return false;
        gain->gain(value);
        return true;
    }

}
