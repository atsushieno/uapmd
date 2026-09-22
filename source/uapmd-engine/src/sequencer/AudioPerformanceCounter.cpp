#include "AudioPerformanceCounter.hpp"

namespace uapmd {
    void AudioPerformanceCounterImpl::setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool AudioPerformanceCounterImpl::enabled() const { return enabled_.load(std::memory_order_relaxed); }
    uint64_t AudioPerformanceCounterImpl::nextBlockNumber() { return ++block_number_; }
    void AudioPerformanceCounterImpl::publish(const AudioProcessingTiming& timing) {
        if (!timings_.try_enqueue(timing))
            dropped_records_.fetch_add(1, std::memory_order_relaxed);
    }
    void AudioPerformanceCounterImpl::recordRealtimeBlock(bool missedDeadline) {
        realtime_blocks_.fetch_add(1, std::memory_order_relaxed);
        if (missedDeadline)
            deadline_misses_.fetch_add(1, std::memory_order_relaxed);
    }
    bool AudioPerformanceCounterImpl::tryDequeue(AudioProcessingTiming& timing) { return timings_.try_dequeue(timing); }
    AudioProcessingTimingCounters AudioPerformanceCounterImpl::counters() const {
        return {realtime_blocks_.load(std::memory_order_relaxed), deadline_misses_.load(std::memory_order_relaxed), dropped_records_.load(std::memory_order_relaxed)};
    }
}
