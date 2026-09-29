#pragma once

#include <atomic>
#include <cstdint>

namespace remidy {

    enum class PerformanceWorkloadHint : int32_t {
        None = 0,
        Increase = 1,
        Spike = 2,
        Reset = 3
    };

    // Process-wide exchange between the audio device's performance hint session and plugin instances that process out of process.
    class PerformanceHintCoordinator {
        static inline std::atomic<bool> enabled_{false};
        static inline std::atomic<int64_t> remote_round_trip_nanos_{0};
        static inline std::atomic<int64_t> remote_processing_nanos_{0};
        static inline std::atomic<float> remote_budget_scale_{0.0f};
        static inline std::atomic<uint32_t> workload_serial_{0};
        static inline std::atomic<int32_t> workload_{0};

    public:
        struct RemoteProcessing {
            int64_t round_trip_nanos{0};
            int64_t processing_nanos{0};
        };

        static bool enabled() { return enabled_.load(std::memory_order_relaxed); }
        static void enabled(bool value) { enabled_.store(value, std::memory_order_relaxed); }

        // Audio thread: an out-of-process instance reports its blocking call time and the remote side's processing time.
        static void addRemoteProcessing(int64_t roundTripNanos, int64_t processingNanos) {
            remote_round_trip_nanos_.fetch_add(roundTripNanos, std::memory_order_relaxed);
            remote_processing_nanos_.fetch_add(processingNanos, std::memory_order_relaxed);
        }
        // Audio thread: the device collects and resets the totals at the end of each callback.
        static RemoteProcessing takeRemoteProcessing() {
            return {remote_round_trip_nanos_.exchange(0, std::memory_order_relaxed),
                    remote_processing_nanos_.exchange(0, std::memory_order_relaxed)};
        }

        // Scale to apply to each remote instance's measured processing time to get its time budget; 0 means unknown.
        static float remoteBudgetScale() { return remote_budget_scale_.load(std::memory_order_relaxed); }
        static void remoteBudgetScale(float value) { remote_budget_scale_.store(value, std::memory_order_relaxed); }

        // Non-realtime: announce a workload change; consumers compare workloadSerial() against the last one they saw.
        static void notifyWorkload(PerformanceWorkloadHint hint) {
            workload_.store(static_cast<int32_t>(hint), std::memory_order_relaxed);
            workload_serial_.fetch_add(1, std::memory_order_release);
        }
        static uint32_t workloadSerial() { return workload_serial_.load(std::memory_order_acquire); }
        static PerformanceWorkloadHint workload() { return static_cast<PerformanceWorkloadHint>(workload_.load(std::memory_order_relaxed)); }
    };
}
