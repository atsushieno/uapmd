#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "uapmd/detail/plugin-api/AudioPluginNodeFeature.hpp"

namespace uapmd {

    template<typename ...TArgs>
    class EventBase {
    public:
        using EventListener = std::function<void(TArgs...)>;

    private:
        std::atomic<EventListenerId> listener_id_counter_{1};
        std::map<EventListenerId, EventListener> listeners_;
        std::mutex listener_mutex_;

    public:
        EventListenerId addListener(EventListener listener) {
            if (!listener)
                return 0;
            std::lock_guard<std::mutex> lock(listener_mutex_);
            auto id = listener_id_counter_++;
            listeners_.emplace(id, std::move(listener));
            return id;
        }

        void removeListener(EventListenerId id) {
            if (id == 0)
                return;
            std::lock_guard<std::mutex> lock(listener_mutex_);
            listeners_.erase(id);
        }

        void notify(TArgs... args) {
            std::vector<EventListener> callbacks;
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                callbacks.reserve(listeners_.size());
                for (auto& kv : listeners_)
                    callbacks.emplace_back(kv.second);
            }
            for (auto& cb : callbacks)
                if (cb)
                    cb(args...);
        }
    };

    using ParameterUpdateEvent = EventBase<int32_t, double>;
    using ParameterMetadataRefreshEvent = EventBase<>;

    class AudioGraphNode {
    public:
        virtual ~AudioGraphNode() = default;

        virtual const std::string& nodeId() const = 0;
        virtual const std::string& nodeType() const = 0;
        virtual const std::string& displayName() const = 0;

        virtual bool bypassed() const = 0;
        virtual void bypassed(bool value) = 0;

        virtual int32_t processAudio(AudioProcessContext& process) = 0;
        virtual uint32_t latencyInSamples() const = 0;
        virtual double tailLengthInSeconds() const = 0;
        virtual remidy::PluginAudioBuses* audioBuses() = 0;

        // Declares the per-bus channel counts this node requires (one entry per bus).
        // An empty vector (the default) means "no opinion" - the owning graph should
        // fall back to its own default bus/channel layout, matching every node that
        // does not change channel topology (e.g. GainNode). Nodes that reshape channel
        // layout (e.g. ChannelMergerNode/ChannelSplitterNode) override these to report
        // their actual bus counts and widths.
        virtual std::vector<uint32_t> requiredAudioInputChannelCounts() const { return {}; }
        virtual std::vector<uint32_t> requiredAudioOutputChannelCounts() const { return {}; }

        virtual ParameterUpdateEvent& parameterUpdateEvent() = 0;
        virtual ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() = 0;
    };

}
