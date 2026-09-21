#pragma once

#include <cstdint>

#include <uapmd-midi-service/uapmd-midi-service.hpp>

namespace uapmd {

class SequencerTrack;

struct TrackAudioProcessingEvent {
    int32_t track_index;
    SequencerTrack& track;
    AudioProcessContext& context;
    int32_t frame_count;
};

// Audio-thread extension point around normal track processing. Implementations
// must not allocate, lock, or change the graph or engine structure here.
class AudioProcessingEventHandler {
public:
    virtual ~AudioProcessingEventHandler() = default;

    // Opt in only if all before callbacks may precede all track graphs and all
    // after callbacks may follow them. Callbacks still run serially on the
    // coordinator. On a failed parallel batch after callbacks are skipped.
    virtual bool supportsParallelTrackProcessing() const noexcept { return false; }

    virtual void beforeTrackProcess(const TrackAudioProcessingEvent&) noexcept {}
    virtual void afterTrackProcess(const TrackAudioProcessingEvent&) noexcept {}
};

} // namespace uapmd
