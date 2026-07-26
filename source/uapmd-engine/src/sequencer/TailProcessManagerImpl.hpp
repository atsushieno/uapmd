#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

class TailProcessManagerImpl final : public TailProcessManager {
    size_t& audio_buffer_size_in_frames_;
    int32_t& sample_rate_;
    std::atomic<bool>& is_playback_active_;
    std::atomic<int64_t>& playback_position_samples_;
    std::atomic<int64_t>& render_playback_position_samples_;

    std::atomic<bool> tail_drain_active_{false};
    std::atomic<int64_t> tail_drain_remaining_samples_{0};
    std::atomic<bool> transport_quiet_pending_{false};
    std::atomic<bool> transport_quiet_{true};
    std::atomic<int64_t> transport_quiet_silence_frames_{0};
    std::atomic<bool> stopped_output_silenced_{false};

    std::atomic<uint64_t> transport_quiet_event_sequence_{0};
    std::atomic<bool> dispatch_stopping_{false};
    std::thread dispatch_thread_;
    std::mutex listener_mutex_;
    TransportQuietListenerId next_listener_id_{1};
    std::unordered_map<TransportQuietListenerId, TransportQuietListener>
        listeners_;

    int64_t alignToQuantum(int64_t samples) const;
    void signalTransportQuiet();
    void dispatchTransportQuietEvents();

public:
    TailProcessManagerImpl(
        size_t& audioBufferSizeInFrames,
        int32_t& sampleRate,
        std::atomic<bool>& isPlaybackActive,
        std::atomic<int64_t>& playbackPositionSamples,
        std::atomic<int64_t>& renderPlaybackPositionSamples);
    ~TailProcessManagerImpl() override;

    bool isTransportQuiet() const override;
    TransportQuietListenerId addTransportQuietListener(
        TransportQuietListener listener) override;
    void removeTransportQuietListener(
        TransportQuietListenerId listenerId) override;

    bool tailDrainActive() const;
    void transportStarted();
    void beginStoppedTransport(int64_t drainFrames);
    void cancelTailProcessing();
    void processAudio(float outputPeak, int32_t frameCount);

    void holdStoppedOutputSilent();
    bool shouldSilenceStoppedOutput() const;
};

} // namespace uapmd
