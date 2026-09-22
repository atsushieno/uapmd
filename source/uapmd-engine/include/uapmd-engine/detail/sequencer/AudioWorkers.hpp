#pragma once

#include "../devices/AudioIODevice.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace uapmd {
    enum class AudioWorkerFault { None, DeadlineExceeded, PluginFailure };

    struct AudioWorkerProgress {
        uint64_t acknowledged_ns{};
        uint64_t retired_ns{};
        int32_t current_track{-1};
        uint32_t completed_jobs{};
    };
    struct AudioTrackProgress {
        uint64_t started_ns{};
        uint64_t finished_ns{};
        uint64_t cpu_ns{};
        int32_t participant{-1};
        int32_t status{};
    };
    struct AudioWorkerProgressSnapshot {
        uint32_t track_count{};
        uint32_t pending_participants{};
        uint32_t completed_jobs{};
        std::array<AudioWorkerProgress, 33> participants{};
        std::array<AudioTrackProgress, 128> tracks{};
    };
    struct AudioWorkerFaultDiagnostic {
        AudioWorkerFault fault{};
        uint64_t block_number{};
        uint32_t worker_count{};
        uint32_t track_count{};
        uint32_t pending_participants{};
        int32_t frame_count{};
        int32_t sample_rate{};
        int32_t failed_track{-1};
        int32_t plugin_status{};
        double elapsed_ms{};
        double dispatch_ms{};
        bool offline{};
        int64_t playback_position_samples{};
        AudioWorkerProgressSnapshot at_fault{};
        bool completion_available{};
        AudioWorkerProgressSnapshot after_completion{};
        bool engine_stopped{};
    };
    class AudioWorkers {
    public:
        virtual ~AudioWorkers() = default;
        virtual bool configure(uint32_t workerCount) = 0;
        virtual bool setThreadSetup(AudioWorkerThreadSetup setup) = 0;
        virtual uint32_t count() const = 0;
        virtual bool stopOnDeadline() const = 0;
        virtual void setStopOnDeadline(bool enabled) = 0;
        virtual void wait() = 0;
        virtual AudioWorkerFault fault() const = 0;
        virtual AudioWorkerFaultDiagnostic diagnostic() = 0;
        virtual void resetFault() = 0;
    };
}
