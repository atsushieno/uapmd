#pragma once

#if defined(__ANDROID__)

#include <atomic>
#include <cstdint>
#include <thread>
#include <sys/types.h>

namespace uapmd {

    // ADPF hint session for the audio callback thread; it reports only the host's own work, excluding time blocked on out-of-process plugins.
    class AndroidPerformanceHintSession {
        std::atomic<void*> session_{nullptr};
        std::atomic<pid_t> callback_thread_{0};
        std::atomic<bool> cancel_open_{false};
        std::thread opener_{};

        // accessed only on the audio thread once the session is published
        pid_t session_thread_{0};
        int64_t session_target_nanos_{0};
        uint32_t workload_serial_{0};
        double remote_round_trip_average_nanos_{0};
        double remote_budget_scale_{0};

    public:
        ~AndroidPerformanceHintSession();

        // Non-realtime: opens the session asynchronously once the first callback has run.
        void start(int64_t initialTargetNanos);
        // Non-realtime: must be called after the stream stopped invoking callbacks.
        void stop();

        // Audio thread
        void beginCallback();
        void endCallback(int64_t elapsedNanos, int64_t periodNanos);
    };
}

#endif // __ANDROID__
