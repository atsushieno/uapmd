#include "TailProcessManagerImpl.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace uapmd {

TailProcessManagerImpl::TailProcessManagerImpl(
    size_t& audioBufferSizeInFrames,
    int32_t& sampleRate,
    std::atomic<bool>& isPlaybackActive,
    std::atomic<int64_t>& playbackPositionSamples,
    std::atomic<int64_t>& renderPlaybackPositionSamples)
    : audio_buffer_size_in_frames_(audioBufferSizeInFrames)
    , sample_rate_(sampleRate)
    , is_playback_active_(isPlaybackActive)
    , playback_position_samples_(playbackPositionSamples)
    , render_playback_position_samples_(renderPlaybackPositionSamples) {
    dispatch_thread_ =
        std::thread([this] { dispatchTransportQuietEvents(); });
}

TailProcessManagerImpl::~TailProcessManagerImpl() {
    dispatch_stopping_.store(true, std::memory_order_release);
    transport_quiet_event_sequence_.fetch_add(1, std::memory_order_release);
    transport_quiet_event_sequence_.notify_one();
    if (dispatch_thread_.joinable())
        dispatch_thread_.join();
}

bool TailProcessManagerImpl::isTransportQuiet() const {
    return transport_quiet_.load(std::memory_order_acquire);
}

TailProcessManager::TransportQuietListenerId
TailProcessManagerImpl::addTransportQuietListener(
    TransportQuietListener listener) {
    if (!listener)
        return 0;
    std::lock_guard lock(listener_mutex_);
    const auto listenerId = next_listener_id_++;
    listeners_.emplace(listenerId, std::move(listener));
    return listenerId;
}

void TailProcessManagerImpl::removeTransportQuietListener(
    TransportQuietListenerId listenerId) {
    std::lock_guard lock(listener_mutex_);
    listeners_.erase(listenerId);
}

int64_t TailProcessManagerImpl::alignToQuantum(int64_t samples) const {
    if (samples <= 0)
        return 0;
    const auto quantum = static_cast<int64_t>(
        audio_buffer_size_in_frames_ > 0
            ? audio_buffer_size_in_frames_
            : 1);
    if (samples > std::numeric_limits<int64_t>::max() - quantum + 1)
        return std::numeric_limits<int64_t>::max();
    return ((samples + quantum - 1) / quantum) * quantum;
}

bool TailProcessManagerImpl::tailDrainActive() const {
    return tail_drain_active_.load(std::memory_order_acquire);
}

void TailProcessManagerImpl::transportStarted() {
    cancelTailProcessing();
    transport_quiet_.store(false, std::memory_order_release);
    stopped_output_silenced_.store(false, std::memory_order_release);
}

void TailProcessManagerImpl::beginStoppedTransport(int64_t drainFrames) {
    transport_quiet_.store(false, std::memory_order_release);
    transport_quiet_silence_frames_.store(0, std::memory_order_release);
    transport_quiet_pending_.store(true, std::memory_order_release);

    const auto alignedFrames = alignToQuantum(drainFrames);
    tail_drain_remaining_samples_.store(
        alignedFrames, std::memory_order_release);
    tail_drain_active_.store(
        alignedFrames > 0, std::memory_order_release);
    if (alignedFrames == 0)
        render_playback_position_samples_.store(
            playback_position_samples_.load(std::memory_order_acquire),
            std::memory_order_release);
}

void TailProcessManagerImpl::cancelTailProcessing() {
    tail_drain_active_.store(false, std::memory_order_release);
    tail_drain_remaining_samples_.store(0, std::memory_order_release);
    transport_quiet_pending_.store(false, std::memory_order_release);
    transport_quiet_silence_frames_.store(0, std::memory_order_release);
}

void TailProcessManagerImpl::processAudio(
    float outputPeak,
    int32_t frameCount) {
    if (tail_drain_active_.load(std::memory_order_acquire)) {
        const auto remaining =
            tail_drain_remaining_samples_.fetch_sub(
                frameCount, std::memory_order_acq_rel) -
            frameCount;
        render_playback_position_samples_.fetch_add(
            frameCount, std::memory_order_release);
        if (remaining <= 0) {
            tail_drain_active_.store(false, std::memory_order_release);
            tail_drain_remaining_samples_.store(
                0, std::memory_order_release);
            render_playback_position_samples_.store(
                playback_position_samples_.load(std::memory_order_acquire),
                std::memory_order_release);
        }
    }

    if (is_playback_active_.load(std::memory_order_acquire) ||
        !transport_quiet_pending_.load(std::memory_order_acquire) ||
        tail_drain_active_.load(std::memory_order_acquire)) {
        transport_quiet_silence_frames_.store(
            0, std::memory_order_release);
        return;
    }

    constexpr float kSilenceThreshold = 0.0001f;
    if (!std::isfinite(outputPeak) || outputPeak > kSilenceThreshold) {
        transport_quiet_silence_frames_.store(
            0, std::memory_order_release);
        return;
    }

    const auto silenceFrames =
        transport_quiet_silence_frames_.fetch_add(
            frameCount, std::memory_order_acq_rel) +
        frameCount;
    const auto requiredSilenceFrames =
        std::max<int64_t>(1, sample_rate_ / 4);
    if (silenceFrames < requiredSilenceFrames ||
        !transport_quiet_pending_.exchange(
            false, std::memory_order_acq_rel))
        return;

    signalTransportQuiet();
}

void TailProcessManagerImpl::holdStoppedOutputSilent() {
    stopped_output_silenced_.store(true, std::memory_order_release);
}

bool TailProcessManagerImpl::shouldSilenceStoppedOutput() const {
    return stopped_output_silenced_.load(std::memory_order_acquire) &&
        !is_playback_active_.load(std::memory_order_acquire);
}

void TailProcessManagerImpl::signalTransportQuiet() {
    transport_quiet_.store(true, std::memory_order_release);
    transport_quiet_event_sequence_.fetch_add(
        1, std::memory_order_release);
    transport_quiet_event_sequence_.notify_one();
}

void TailProcessManagerImpl::dispatchTransportQuietEvents() {
    uint64_t observedSequence = 0;
    while (true) {
        transport_quiet_event_sequence_.wait(
            observedSequence, std::memory_order_acquire);
        if (dispatch_stopping_.load(std::memory_order_acquire))
            return;
        observedSequence = transport_quiet_event_sequence_.load(
            std::memory_order_acquire);

        std::vector<TransportQuietListener> listeners;
        {
            std::lock_guard lock(listener_mutex_);
            listeners.reserve(listeners_.size());
            for (const auto& [_, listener] : listeners_)
                listeners.push_back(listener);
        }
        for (const auto& listener : listeners)
            listener();
    }
}

} // namespace uapmd
