#pragma once

#include <string_view>

namespace uapmd {

// Non-audio-thread transport lifecycle extension point. Extensions subscribe
// once and react to both transport and recording transitions.
class PlaybackEngineExtension {
public:
    virtual ~PlaybackEngineExtension() = default;
    virtual std::string_view extensionId() const = 0;
    virtual void playbackStarted() {}
    virtual void playbackStopped() {}
    virtual void recordingStarted() {}
    virtual void recordingStopped() {}
};

} // namespace uapmd
