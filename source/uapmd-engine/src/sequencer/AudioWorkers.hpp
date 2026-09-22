#pragma once

#include "AudioTrackWorkerPool.hpp"
#include "readerwriterqueue.h"

#include <cstddef>
#include <mutex>

namespace uapmd {
    class AudioWorkersImpl final : public AudioWorkers {
    public:
        bool configure(uint32_t workerCount) override;
        bool configure(uint32_t workerCount, size_t trackCount);
        bool setThreadSetup(AudioWorkerThreadSetup setup) override;
        bool setThreadSetup(AudioWorkerThreadSetup setup, size_t trackCount);
        bool hasThreadSetup() const;
        uint32_t count() const override;
        bool enabled() const;
        bool busy() const;
        void waitUntilIdle() const;
        void wait() override;
        void resizeJobs(size_t trackCount);
        AudioTrackJob& job(size_t index);
        void start(uint32_t jobCount, bool captureTiming);
        void participate(AudioTrackWorkerPool::Clock::time_point deadline);
        bool completeBefore(AudioTrackWorkerPool::Clock::time_point deadline) const;
        uint32_t pendingParticipants() const;
        AudioWorkerProgressSnapshot progressSnapshot() const;
        AudioWorkerFault fault() const override;
        void setFault(AudioWorkerFault fault);
        void clearFault();
        bool stopOnDeadline() const override;
        void setStopOnDeadline(bool enabled) override;
        bool batchPending() const;
        void setBatchPending(bool pending);
        bool retireLateBatch();
        AudioWorkerFaultDiagnostic& diagnosticForWrite();
        void publishDiagnostic();
        AudioWorkerFaultDiagnostic diagnostic() override;
        AudioWorkerFaultDiagnostic diagnostic(bool callbackExited);
        void resetFault() override;
        void setResetHandler(std::function<void()> handler);
        void setWaitHandler(std::function<void()> handler);
        void reportDiagnostics();

    private:
        bool configurePool(uint32_t workerCount, const AudioWorkerThreadSetup& setup, size_t trackCount);
        std::unique_ptr<AudioTrackWorkerPool> pool_;
        std::vector<AudioTrackJob> jobs_;
        AudioWorkerThreadSetup thread_setup_;
        std::atomic<AudioWorkerFault> fault_{AudioWorkerFault::None};
        std::atomic<bool> stop_on_deadline_{false};
        bool batch_pending_{};
        AudioWorkerFaultDiagnostic diagnostic_;
        moodycamel::ReaderWriterQueue<AudioWorkerFaultDiagnostic> diagnostics_{32};
        std::atomic<uint32_t> dropped_diagnostics_{0};
        AudioWorkerFaultDiagnostic last_diagnostic_;
        std::mutex diagnostic_mutex_;
        std::function<void()> reset_handler_;
        std::function<void()> wait_handler_;
    };
}
