#pragma once

#include <cstdint>
#include <limits>

#include <uapmd/uapmd.hpp>

namespace uapmd {

inline constexpr int32_t kMasterTrackIndex = std::numeric_limits<int32_t>::min();
using uapmd_track_index_t = int32_t;

class SequencerEngine;
class SequencerTrack;

class TrackAudioProcessorExtension {
public:
    virtual ~TrackAudioProcessorExtension() = default;

    virtual bool shouldProcessAudio(
        SequencerEngine& engine,
        uapmd_track_index_t trackIndex,
        SequencerTrack& track,
        AudioProcessContext& context) = 0;

    virtual void processAudio(
        SequencerEngine& engine,
        uapmd_track_index_t trackIndex,
        SequencerTrack& track,
        AudioProcessContext& context) = 0;

    // Called from a non-audio thread after a change that can alter a track's
    // rendered output, such as a plugin parameter write.
    virtual void audioContentChanged(SequencerEngine&, uapmd_track_index_t) {}
};

} // namespace uapmd
