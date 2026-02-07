
#include <umppi/umppi.hpp>
#include "concurrentqueue.h"

#include <atomic>
#include <cstring>
#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginNodeImpl : public AudioPluginNode {
        int32_t instance_id_;
        AudioPluginInstanceAPI* instance_;
        moodycamel::ConcurrentQueue<umppi::Ump> queue_;
        std::vector<umppi::Ump> pending_events_;
        std::atomic<bool> queue_reading_{false};
        std::function<void()> on_delete_;
        ParameterUpdateEvent parameter_update_event_;
        ParameterMetadataRefreshEvent parameter_metadata_refresh_event_;
        remidy::EventListenerId parameter_listener_token_{0};
        remidy::EventListenerId metadata_listener_token_{0};

    public:
        AudioPluginNodeImpl(
            int32_t instanceId,
            AudioPluginInstanceAPI* instance,
            size_t eventBufferSizeInBytes,
            std::function<void()>&& onDelete
        ) : instance_id_(instanceId),
            instance_(instance),
            queue_(eventBufferSizeInBytes),
            on_delete_(std::move(onDelete)) {
            // Register parameter change listener directly with the plugin
            if (instance_ && instance_->parameterSupport()) {
                parameter_listener_token_ = instance_->parameterSupport()->parameterChangeEvent().addListener(
                    [this](uint32_t paramIndex, double plainValue) {
                        parameter_update_event_.notify(static_cast<int32_t>(paramIndex), plainValue);
                    }
                );
                metadata_listener_token_ = instance_->parameterSupport()->parameterMetadataChangeEvent().addListener(
                    [this]() {
                        parameter_metadata_refresh_event_.notify();
                    }
                );
            }
        }

        ~AudioPluginNodeImpl() override {
            // Unregister parameter listeners
            if (instance_ && instance_->parameterSupport()) {
                if (parameter_listener_token_ != 0)
                    instance_->parameterSupport()->parameterChangeEvent().removeListener(parameter_listener_token_);
                if (metadata_listener_token_ != 0)
                    instance_->parameterSupport()->parameterMetadataChangeEvent().removeListener(metadata_listener_token_);
            }
            if (on_delete_)
                on_delete_();
        }

        int32_t instanceId() const override {
            return instance_id_;
        }

        AudioPluginInstanceAPI* instance() override {
            return instance_;
        }

        ParameterUpdateEvent& parameterUpdateEvent() override {
            return parameter_update_event_;
        }

        ParameterMetadataRefreshEvent& parameterMetadataRefreshEvent() override {
            return parameter_metadata_refresh_event_;
        }

        bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) override {
            auto* bytes = static_cast<const uint8_t*>(events);
            size_t offset = 0;
            while (offset + sizeof(uint32_t) <= size) {
                auto* words = reinterpret_cast<const uint32_t*>(bytes + offset);
                auto messageType = static_cast<uint8_t>(words[0] >> 28);
                auto wordCount = umppi::umpSizeInInts(messageType);
                auto messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
                if (offset + messageSize > size)
                    break;
                umppi::Ump u128(words[0],
                                wordCount > 1 ? words[1] : 0,
                                wordCount > 2 ? words[2] : 0,
                                wordCount > 3 ? words[3] : 0);
                if (!queue_.enqueue(u128))
                    return false;
                offset += messageSize;
            }
            return true;
        }

        // Internal methods for AudioPluginGraph to use
        void drainQueueToPending() {
            queue_reading_.exchange(true);
            umppi::Ump u128;
            while (queue_.try_dequeue(u128)) {
                pending_events_.push_back(u128);
            }
            queue_reading_.exchange(false);
        }

        size_t fillEventBufferForGroup(EventSequence& eventIn, uint8_t group) {
            auto* messages = static_cast<uint8_t*>(eventIn.getMessages());
            size_t position = eventIn.position();
            const auto capacity = eventIn.maxMessagesInBytes();

            auto it = pending_events_.begin();
            while (it != pending_events_.end()) {
                const auto& ump = *it;
                const auto msgGroup = ump.getGroup();
                if (group != 0xFF && msgGroup != group) {
                    ++it;
                    continue;
                }
                const auto messageSize = static_cast<size_t>(ump.getSizeInBytes());
                if (position + messageSize > capacity) {
                    break;
                }
                auto ints = ump.toInts();
                std::memcpy(messages + position, ints.data(), messageSize);
                position += messageSize;
                it = pending_events_.erase(it);
            }
            eventIn.position(position);
            return position;
        }

        friend class AudioPluginGraphImpl;
    };

}
