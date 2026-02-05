#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "../plugin-api/AudioPluginInstanceAPI.hpp"

namespace uapmd {

    using EventListenerId = int64_t;

    // Generic event system for multiple listeners
    // Not RT-safe due to mutex usage
    // Listeners are called in the order they were registered (insertion order)
    template<typename ...TArgs>
    class EventBase {
    public:
        using EventListener = std::function<void(TArgs...)>;

    private:
        std::atomic<EventListenerId> listener_id_counter_{1};
        std::map<EventListenerId, EventListener> listeners_; // std::map maintains order by key
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

    // Event for parameter updates: (parameterIndex, value)
    using ParameterUpdateEvent = EventBase<int32_t, double>;

    // AudioPluginNode wraps a plugin instance with its own event queue.
    // This allows per-instance event routing instead of per-track routing.
    // Managed internally by AudioPluginGraph.
    class AudioPluginNode {
    public:
        virtual ~AudioPluginNode() = default;

        virtual int32_t instanceId() const = 0;
        virtual AudioPluginInstanceAPI* instance() = 0;

        // Schedule UMP events to this plugin instance's queue
        virtual bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) = 0;

        // Parameter update event - allows multiple consumers to listen for parameter changes
        virtual ParameterUpdateEvent& parameterUpdateEvent() = 0;
    };

}
