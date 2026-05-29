#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

    class AudioPluginNode : public AudioPluginNodeFeature {
    public:
        ~AudioPluginNode() override = default;

        virtual int32_t instanceId() const = 0;
        virtual AudioPluginInstanceAPI* instance() override = 0;
        virtual bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) override = 0;
        virtual void sendAllNotesOff() = 0;
        virtual void requestStopFlush() = 0;
        virtual ParameterUpdateEvent& parameterUpdateEvent() = 0;
        virtual ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() = 0;
    };

}
