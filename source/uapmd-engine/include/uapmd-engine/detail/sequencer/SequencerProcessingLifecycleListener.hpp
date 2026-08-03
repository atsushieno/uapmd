#pragma once

#include <cstdint>

#include "TrackAudioProcessorExtension.hpp"

namespace uapmd {

enum class SequencerTransportTransition {
    PositionChanged,
    Started,
    Stopped,
    Paused,
    Resumed,
};

// Non-audio-thread extension point for state that backs audio processing.
// Notifications that change RT-owned storage must be emitted only after the
// engine has excluded the audio callback from that storage.
class SequencerProcessingLifecycleListener {
public:
    virtual ~SequencerProcessingLifecycleListener() = default;

    virtual void trackAdded(uapmd_track_index_t) {}
    virtual void trackRemoved(uapmd_track_index_t) {}
    virtual void pluginInstanceWillBeDestroyed(int32_t) {}
    virtual void audioProcessingConfigurationChanged() {}
    virtual void pluginGraphChanged() {}
    virtual void graphTimingChanged(bool isPlaybackActive) {}
    virtual void trackProcessingStateReset(uapmd_track_index_t) {}
    virtual void processingStateReset() {}
    virtual void transportTransition(
        SequencerTransportTransition,
        int64_t audiblePositionSamples) {}
};

} // namespace uapmd
