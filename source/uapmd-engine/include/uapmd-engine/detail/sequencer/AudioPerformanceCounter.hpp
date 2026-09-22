#pragma once

#include <cstdint>

namespace uapmd {
    enum class AudioProcessingStage { Preparation, Track, Tracks, MixAndMaster, PostProcessing, Callback };
    struct AudioProcessingTiming {
        uint64_t block_number{};
        AudioProcessingStage stage{};
        int32_t track_index{-1};
        int32_t frame_count{};
        int32_t sample_rate{};
        uint64_t duration_nanoseconds{};
        bool offline{};
    };
    struct AudioProcessingTimingCounters {
        uint32_t realtime_blocks{};
        uint32_t deadline_misses{};
        uint32_t dropped_records{};
    };
    class AudioPerformanceCounter {
    public:
        virtual ~AudioPerformanceCounter() = default;
        virtual void setEnabled(bool enabled) = 0;
        virtual bool tryDequeue(AudioProcessingTiming& timing) = 0;
        virtual AudioProcessingTimingCounters counters() const = 0;
    };
}
