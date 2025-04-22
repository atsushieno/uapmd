
#include "uapmd/uapmd.hpp"
#include "cmidi2.h"
#include "concurrentqueue.h"

namespace uapmd {
    class AudioPluginTrack::Impl {
        EventSequence midi;
        moodycamel::ConcurrentQueue<cmidi2_ump128_t> queue;
        std::atomic<bool> queue_reading{false};


    public:
        explicit Impl(size_t eventBufferSizeInBytes) :
            midi(eventBufferSizeInBytes),
            queue(eventBufferSizeInBytes) {
        }

        bool bypass{false};
        bool frozen{false};
        AudioPluginGraph graph{};
        bool scheduleEvents(uapmd_timestamp_t timestamp, void* events, size_t size);
        int32_t processAudio(AudioProcessContext& process);
    };

    int32_t AudioPluginTrack::Impl::processAudio(AudioProcessContext& process) {
        queue_reading.exchange(true);
        auto& eventIn = process.eventIn();
        auto messages = (uint8_t*) eventIn.getMessages();
        cmidi2_ump128_t u128;
        size_t totalBytes = 0;
        while (queue.try_dequeue(u128)) {
            const auto u = static_cast<cmidi2_ump *>(static_cast<void *>(&u128));
            const size_t retrievedSize = cmidi2_ump_get_message_size_bytes(u);
            memcpy(messages + totalBytes, u, retrievedSize);
            //Logger::global()->logInfo("UMP: %x %x ... %d bytes at %x", u128.p1, u128.p2, retrievedSize, (uint64_t) (void*) messages);
            totalBytes += retrievedSize;
        }
        eventIn.position(totalBytes);
        //if (totalBytes > 0)
        //    Logger::global()->logInfo("  total bytes: %d", totalBytes);
        const auto ret = graph.processAudio(process);
        eventIn.position(eventIn.position() - totalBytes);
        queue_reading.exchange(false);
        return ret;
    }

    bool AudioPluginTrack::Impl::scheduleEvents(uapmd_timestamp_t timestamp, void *events, size_t size) {
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

}
