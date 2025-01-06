
#include "uapmd/uapmd.hpp"
#include "cmidi2.h"
#include "ring_buffer/ring_buffer.h"

namespace uapmd {
    class AudioPluginTrack::Impl {
        EventSequence midi;
        // We use SPSC ring buffer here, so writers need to wait while reading
        Ring_Buffer_Ex<true> queue;
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
        size_t retrievedSize = queue.size_used();
        queue.get((uint8_t*) eventIn.getMessages(), retrievedSize);
        eventIn.position(retrievedSize);
        auto ret = graph.processAudio(process);
        eventIn.position(0);
        queue_reading.exchange(false);
        return ret;
    }

    bool AudioPluginTrack::Impl::scheduleEvents(uapmd_timestamp_t timestamp, void *events, size_t size) {
        queue_reading.wait(true);
        return queue.put((uint8_t*) events, size);
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
