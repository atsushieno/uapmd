#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <uapmd-plugin-hosting/uapmd-plugin-hosting.hpp>
#include "PlaybackEngineExtension.hpp"

namespace uapmd {

class SequencerEngine;

// Owns the capture session for one selected MIDI clip. MIDI routing remains in
// SequencerEngine; it forwards matching input messages here while recording.
class MidiRecorder final : public PlaybackEngineExtension {
public:
    struct Target {
        std::string trackId;
        int32_t clipId{-1};
        int64_t startSample{0};
    };

    struct Event {
        int64_t samplePosition{0};
        std::vector<uapmd_ump_t> words;
    };

    explicit MidiRecorder(SequencerEngine& engine);
    std::string_view extensionId() const override { return "midi-recorder"; }
    bool start(Target target);
    void stop();
    void cancel();
    bool isRecording() const;
    Target target() const;

    // MIDI input callbacks are not run on the audio callback. This short
    // critical section only copies the already-received UMP into the session.
    void record(std::string_view trackId, const uapmd_ump_t* ump,
                size_t sizeInBytes, int64_t samplePosition);
    void playbackStopped() override;
    void recordingStopped() override;

private:
    mutable std::mutex mutex_;
    bool recording_{false};
    Target target_;
    int64_t start_sample_{0};
    int32_t sample_rate_{48000};
    std::vector<Event> events_;
    SequencerEngine& engine_;
    std::vector<Event> takeEvents();
    void commit();
};

} // namespace uapmd
