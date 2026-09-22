#pragma once

#include <uapmd-engine/detail/sequencer/AudioPerformanceCounter.hpp>
#include "readerwriterqueue.h"

#include <atomic>

namespace uapmd {
    class AudioPerformanceCounterImpl final : public AudioPerformanceCounter {
    public:
        void setEnabled(bool enabled) override;
        bool enabled() const;
        uint64_t nextBlockNumber();
        void publish(const AudioProcessingTiming& timing);
        void recordRealtimeBlock(bool missedDeadline);
        bool tryDequeue(AudioProcessingTiming& timing) override;
        AudioProcessingTimingCounters counters() const override;

    private:
        std::atomic<bool> enabled_{false};
        moodycamel::ReaderWriterQueue<AudioProcessingTiming> timings_{4096};
        std::atomic<uint32_t> realtime_blocks_{0};
        std::atomic<uint32_t> deadline_misses_{0};
        std::atomic<uint32_t> dropped_records_{0};
        uint64_t block_number_{};
    };
}
