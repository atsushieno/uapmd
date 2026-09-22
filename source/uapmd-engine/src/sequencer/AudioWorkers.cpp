#include "AudioWorkers.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <utility>

namespace uapmd {
    bool AudioWorkersImpl::configurePool(uint32_t workerCount, const AudioWorkerThreadSetup& setup, size_t trackCount) {
        if (workerCount > 32)
            return false;
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        if (workerCount != 0)
            return false;
#endif
        try {
            auto pool = workerCount ? std::make_unique<AudioTrackWorkerPool>(workerCount, setup) : nullptr;
            jobs_.resize(trackCount);
            pool_ = std::move(pool);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool AudioWorkersImpl::configure(uint32_t workerCount) { return configure(workerCount, jobs_.size()); }
    bool AudioWorkersImpl::configure(uint32_t workerCount, size_t trackCount) {
        return configurePool(workerCount, thread_setup_, trackCount);
    }

    bool AudioWorkersImpl::setThreadSetup(AudioWorkerThreadSetup setup) { return setThreadSetup(std::move(setup), jobs_.size()); }
    bool AudioWorkersImpl::setThreadSetup(AudioWorkerThreadSetup setup, size_t trackCount) {
        if (!configurePool(count(), setup, trackCount))
            return false;
        thread_setup_ = std::move(setup);
        return true;
    }

    bool AudioWorkersImpl::hasThreadSetup() const { return static_cast<bool>(thread_setup_); }
    uint32_t AudioWorkersImpl::count() const { return pool_ ? pool_->workerCount() : 0; }
    bool AudioWorkersImpl::enabled() const { return pool_ != nullptr; }
    bool AudioWorkersImpl::busy() const { return pool_ && pool_->busy(); }
    void AudioWorkersImpl::waitUntilIdle() const { if (pool_) pool_->waitUntilIdle(); }
    void AudioWorkersImpl::wait() { waitUntilIdle(); if (wait_handler_) wait_handler_(); reportDiagnostics(); }
    void AudioWorkersImpl::resizeJobs(size_t trackCount) { jobs_.resize(trackCount); }
    AudioTrackJob& AudioWorkersImpl::job(size_t index) { return jobs_[index]; }
    void AudioWorkersImpl::start(uint32_t jobCount, bool captureTiming) { pool_->start(jobs_.data(), jobCount, captureTiming); }
    void AudioWorkersImpl::participate(AudioTrackWorkerPool::Clock::time_point deadline) { pool_->participate(deadline); }
    bool AudioWorkersImpl::completeBefore(AudioTrackWorkerPool::Clock::time_point deadline) const { return pool_->completeBefore(deadline); }
    uint32_t AudioWorkersImpl::pendingParticipants() const { return pool_->pendingParticipants(); }
    AudioWorkerProgressSnapshot AudioWorkersImpl::progressSnapshot() const { return pool_->progressSnapshot(); }
    AudioWorkerFault AudioWorkersImpl::fault() const { return fault_.load(std::memory_order_acquire); }
    void AudioWorkersImpl::setFault(AudioWorkerFault fault) { fault_.store(fault, std::memory_order_release); }
    void AudioWorkersImpl::clearFault() { setFault(AudioWorkerFault::None); }
    bool AudioWorkersImpl::stopOnDeadline() const { return stop_on_deadline_.load(std::memory_order_relaxed); }
    void AudioWorkersImpl::setStopOnDeadline(bool enabled) { stop_on_deadline_.store(enabled, std::memory_order_relaxed); }
    bool AudioWorkersImpl::batchPending() const { return batch_pending_; }
    void AudioWorkersImpl::setBatchPending(bool pending) { batch_pending_ = pending; }
    bool AudioWorkersImpl::retireLateBatch() {
        if (!batch_pending_)
            return false;
        diagnostic_.after_completion = progressSnapshot();
        diagnostic_.completion_available = true;
        bool pluginFailed = false;
        for (uint32_t i = 0; i < diagnostic_.track_count; ++i)
            if (jobs_[i].status != 0) {
                diagnostic_.fault = AudioWorkerFault::PluginFailure;
                diagnostic_.failed_track = static_cast<int32_t>(i);
                diagnostic_.plugin_status = jobs_[i].status;
                diagnostic_.engine_stopped = true;
                setFault(AudioWorkerFault::PluginFailure);
                pluginFailed = true;
                break;
            }
        publishDiagnostic();
        batch_pending_ = false;
        return pluginFailed;
    }
    AudioWorkerFaultDiagnostic& AudioWorkersImpl::diagnosticForWrite() { return diagnostic_; }
    void AudioWorkersImpl::publishDiagnostic() {
        if (!diagnostics_.try_enqueue(diagnostic_))
            dropped_diagnostics_.fetch_add(1, std::memory_order_relaxed);
    }

    AudioWorkerFaultDiagnostic AudioWorkersImpl::diagnostic() { return diagnostic(true); }
    AudioWorkerFaultDiagnostic AudioWorkersImpl::diagnostic(bool callbackExited) {
        reportDiagnostics();
        std::lock_guard lock(diagnostic_mutex_);
        if (fault() != AudioWorkerFault::None && callbackExited && enabled() && !busy() &&
            last_diagnostic_.block_number == diagnostic_.block_number && last_diagnostic_.engine_stopped &&
            !last_diagnostic_.completion_available) {
            last_diagnostic_.after_completion = progressSnapshot();
            last_diagnostic_.completion_available = true;
        }
        return last_diagnostic_;
    }

    void AudioWorkersImpl::resetFault() { if (reset_handler_) reset_handler_(); clearFault(); }
    void AudioWorkersImpl::setResetHandler(std::function<void()> handler) { reset_handler_ = std::move(handler); }
    void AudioWorkersImpl::setWaitHandler(std::function<void()> handler) { wait_handler_ = std::move(handler); }
    void AudioWorkersImpl::reportDiagnostics() {
        std::lock_guard lock(diagnostic_mutex_);
        AudioWorkerFaultDiagnostic diagnostic;
        for (uint32_t count = 0; count < 32 && diagnostics_.try_dequeue(diagnostic); ++count) {
            const bool completionOnly = diagnostic.completion_available &&
                diagnostic.block_number == last_diagnostic_.block_number && diagnostic.fault == last_diagnostic_.fault;
            last_diagnostic_ = diagnostic;
            if (completionOnly)
                continue;
            const double periodMs = diagnostic.sample_rate > 0 ? 1000.0 * diagnostic.frame_count / diagnostic.sample_rate : 0.0;
            remidy::Logger::global()->logError(
                "%s: %s; block=%llu, mode=%s, workers=%u (+ coordinator), tracks=%u, "
                "frames=%d, sample_rate=%d Hz, callback_elapsed=%.3f ms, block_period=%.3f ms, "
                "worker_budget=%.3f ms (%s), dispatch_at=%.3f ms, "
                "pending_workers=%u, failed_track=%d (zero-based; -1=unknown), plugin_status=%d. "
                "%s Try Serial/fewer workers or a larger audio buffer if deadline overruns recur.",
                diagnostic.engine_stopped ? "Audio engine stopped" : "Audio deadline overrun",
                diagnostic.fault == AudioWorkerFault::DeadlineExceeded ? "audio worker completion deadline exceeded" : "plugin processing failed",
                static_cast<unsigned long long>(diagnostic.block_number), diagnostic.offline ? "offline" : "realtime",
                diagnostic.worker_count, diagnostic.track_count, diagnostic.frame_count, diagnostic.sample_rate,
                diagnostic.elapsed_ms, periodMs, diagnostic.offline ? 0.0 : 0.8 * periodMs,
                diagnostic.offline ? "disabled offline" : "80% of block", diagnostic.dispatch_ms,
                diagnostic.pending_participants, diagnostic.failed_track, diagnostic.plugin_status,
                diagnostic.engine_stopped ? "Output is silenced; restart the audio engine to resume."
                    : "Engine remains enabled; output is silenced until late workers finish, then processing resumes automatically.");
            uint32_t acknowledged = 0;
            for (uint32_t i = 1; i <= diagnostic.worker_count; ++i)
                if (diagnostic.at_fault.participants[i].acknowledged_ns != 0)
                    ++acknowledged;
            remidy::Logger::global()->logError(
                "Audio worker snapshot: block=%llu, playback_samples=%lld, workers_acknowledged=%u/%u, "
                "jobs_completed=%u/%u. Query get_audio_worker_diagnostics via MCP for per-worker/track details.",
                static_cast<unsigned long long>(diagnostic.block_number), static_cast<long long>(diagnostic.playback_position_samples),
                acknowledged, diagnostic.worker_count, diagnostic.at_fault.completed_jobs, diagnostic.track_count);
        }
        const auto dropped = dropped_diagnostics_.exchange(0, std::memory_order_relaxed);
        if (dropped != 0)
            remidy::Logger::global()->logError("Audio worker diagnostics: %u incident/completion reports dropped because the diagnostic queue was full.", dropped);
    }
}
