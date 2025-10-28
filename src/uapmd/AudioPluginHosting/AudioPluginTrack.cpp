
#include "uapmd/uapmd.hpp"
#include "cmidi2.h"
#include "concurrentqueue.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace uapmd {
    class AudioPluginTrack::Impl {
        EventSequence midi;
        moodycamel::ConcurrentQueue<cmidi2_ump128_t> queue;
        std::atomic<bool> queue_reading{false};
        std::vector<cmidi2_ump128_t> pending_events;
        std::function<uint8_t(int32_t)> group_resolver;
        std::function<void(int32_t, const uapmd_ump_t*, size_t)> event_output_callback;

        size_t fillEventBufferForGroup(EventSequence& eventIn, uint8_t group) {
            auto* messages = static_cast<uint8_t*>(eventIn.getMessages());
            size_t position = 0;
            const auto capacity = eventIn.maxMessagesInBytes();

            auto it = pending_events.begin();
            while (it != pending_events.end()) {
                auto* msg = reinterpret_cast<cmidi2_ump*>(std::addressof(*it));
                const auto msgGroup = cmidi2_ump_get_group(msg);
                if (group != 0xFF && msgGroup != group) {
                    ++it;
                    continue;
                }
                const auto messageSize = cmidi2_ump_get_message_size_bytes(msg);
                if (position + messageSize > capacity) {
                    break;
                }
                std::memcpy(messages + position, msg, messageSize);
                position += messageSize;
                it = pending_events.erase(it);
            }
            eventIn.position(position);
            return position;
        }

        void appendEventOutToPending(EventSequence& eventOut) {
            const auto bytes = eventOut.position();
            if (bytes == 0)
                return;

            auto* messages = static_cast<uint8_t*>(eventOut.getMessages());
            size_t offset = 0;
            while (offset < bytes) {
                auto* msg = reinterpret_cast<cmidi2_ump*>(messages + offset);
                auto size = cmidi2_ump_get_message_size_bytes(msg);
                auto* words = reinterpret_cast<uint32_t*>(messages + offset);
                cmidi2_ump128_t u128{
                    words[0],
                    size > sizeof(uint32_t) ? words[1] : 0,
                    size > sizeof(uint32_t) * 2 ? words[2] : 0,
                    size > sizeof(uint32_t) * 3 ? words[3] : 0
                };
                pending_events.push_back(u128);
                offset += size;
            }
        }

    public:
        explicit Impl(size_t eventBufferSizeInBytes) :
            midi(eventBufferSizeInBytes),
            queue(eventBufferSizeInBytes) {
        }

        bool bypass{false};
        bool frozen{false};
        AudioPluginGraph graph{};

        bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size) {
            CMIDI2_UMP_SEQUENCE_FOREACH(events, size, iter) {
                auto u = (cmidi2_ump*) iter;
                auto u32 = (uint32_t*) iter;
                auto uSize = cmidi2_ump_get_message_size_bytes(u);
                cmidi2_ump128_t u128{u32[0], uSize > 4 ? u32[1] : 0, uSize > 8 ? u32[2] : 0, uSize > 8 ? u32[3] : 0};
                if (!queue.enqueue(u128))
                    return false;
            }
            return true;
        }

        int32_t processAudio(AudioProcessContext& process) {
            queue_reading.exchange(true);
            cmidi2_ump128_t u128;
            while (queue.try_dequeue(u128)) {
                pending_events.push_back(u128);
            }
            queue_reading.exchange(false);

            process.clearAudioOutputs();

            auto plugins = graph.plugins();
            if (plugins.empty())
                return 0;

            for (size_t idx = 0; idx < plugins.size(); ++idx) {
                auto* plugin = plugins[idx];
                if (!plugin)
                    continue;

                auto instanceId = plugin->instanceId();
                uint8_t group = 0xFF;
                if (group_resolver)
                    group = group_resolver(instanceId);

                auto& eventIn = process.eventIn();
                eventIn.position(0);
                fillEventBufferForGroup(eventIn, group);

                auto status = plugin->processAudio(process);
                if (status != 0)
                    return status;

                auto& eventOut = process.eventOut();
                if (eventOut.position() > 0) {
                    appendEventOutToPending(eventOut);
                    if (event_output_callback) {
                        event_output_callback(
                            instanceId,
                            static_cast<uapmd_ump_t*>(eventOut.getMessages()),
                            eventOut.position()
                        );
                    }
                    eventOut.position(0);
                }

                if (idx + 1 < plugins.size()) {
                    process.advanceToNextNode();
                }
            }

            return 0;
        }

        void setGroupResolver(std::function<uint8_t(int32_t)> resolver) {
            group_resolver = std::move(resolver);
        }

        void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) {
            event_output_callback = std::move(callback);
        }
    };

    AudioPluginTrack::AudioPluginTrack(size_t eventBufferSizeInBytes) {
        impl = new Impl(eventBufferSizeInBytes);
    }

    AudioPluginTrack::~AudioPluginTrack() {
        delete impl;
    }

    AudioPluginGraph& AudioPluginTrack::graph() {
        return impl->graph;
    }

    bool AudioPluginTrack::bypassed() { return impl->bypass; }

    bool AudioPluginTrack::frozen() { return impl->frozen; }

    void AudioPluginTrack::bypassed(bool value) { impl->bypass = value; }

    void AudioPluginTrack::frozen(bool value) { impl->frozen = value; }

    int32_t AudioPluginTrack::processAudio(AudioProcessContext& process) {
        return impl->processAudio(process);
    }

    bool AudioPluginTrack::scheduleEvents(uapmd_timestamp_t timestamp, void *events, size_t size) {
        return impl->scheduleEvents(timestamp, events, size);
    }

    void AudioPluginTrack::setGroupResolver(std::function<uint8_t(int32_t)> resolver) {
        impl->setGroupResolver(std::move(resolver));
    }

    void AudioPluginTrack::setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) {
        impl->setEventOutputCallback(std::move(callback));
    }

}
