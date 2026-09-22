#pragma once

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <time.h>
#include <thread>
#include <vector>
#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd {

// Storage belongs to the engine and cannot be resized until every admitted
// participant has retired. Sleeping workers are excluded by the admission gate.
struct AudioTrackJob {
    uapmd_graph::AudioPluginGraph* graph{};
    AudioProcessContext* context{};
    uint64_t duration_nanoseconds{};
    int32_t status{};
};

// A single atomic combines the batch identity, admission closure, and ownership
// count. Workers must enter with their observed ticket before reading any batch
// storage. Closing the gate rejects sleepers; changing its identity rejects a
// stale admission attempt even after storage has been reused for another batch.
class AudioBatchAdmission {
public:
    using Ticket = uint64_t;

    void begin() {
        // Coordinator only, after busy() became false. Begin owns one reference
        // for the coordinator; no worker references are reserved in advance.
        const auto next = (state_.load(std::memory_order_relaxed) & ~FLAGS) + GENERATION;
        state_.store(next | 1, std::memory_order_release);
    }
    Ticket ticket() const { return state_.load(std::memory_order_acquire); }
    static bool open(Ticket ticket) { return (ticket & CLOSED) == 0; }
    bool tryEnter(Ticket observed) {
        return open(observed) && state_.compare_exchange_strong(
            observed, observed + 1, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void close() { state_.fetch_or(CLOSED, std::memory_order_acq_rel); }
    void leave() { state_.fetch_sub(1, std::memory_order_acq_rel); }
    uint32_t activeCount() const { return static_cast<uint32_t>(ticket() & COUNT); }
    bool busy() const { return (ticket() & FLAGS) != CLOSED; }

private:
    // Up to 32 workers plus the coordinator. Identity wraps after 2^57 batches.
    static constexpr Ticket COUNT = 63;
    static constexpr Ticket CLOSED = 64;
    static constexpr Ticket FLAGS = COUNT | CLOSED;
    static constexpr Ticket GENERATION = FLAGS + 1;
    static_assert(std::atomic<Ticket>::is_always_lock_free);
    std::atomic<Ticket> state_{CLOSED};
};

class AudioTrackWorkerPool {
public:
    using Clock = std::chrono::steady_clock;

    explicit AudioTrackWorkerPool(uint32_t workerCount, AudioWorkerThreadSetup setup = {})
        : thread_setup_(std::move(setup)) {
        static_assert(std::atomic<uint32_t>::is_always_lock_free);
        workers_.reserve(workerCount);
        try {
            for (uint32_t i = 0; i < workerCount; ++i)
                workers_.emplace_back([this, i] { runWorker(i + 1); });
            while (started_.load(std::memory_order_acquire) != workerCount)
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            if (setup_failed_.load(std::memory_order_relaxed))
                throw std::runtime_error("Audio worker thread setup failed");
        } catch (...) {
            stop();
            throw;
        }
    }

    ~AudioTrackWorkerPool() {
        waitUntilIdle();
        stop();
    }

    uint32_t pendingParticipants() const { return admission_.activeCount(); }
    bool busy() const { return admission_.busy(); }
    uint32_t workerCount() const { return static_cast<uint32_t>(workers_.size()); }

    // Coordinator only, with no previous batch in flight. Publication uses only
    // atomics: idle workers poll, so the audio callback performs no OS wake call.
    void start(AudioTrackJob* jobs, uint32_t count, bool captureTiming) {
        batch_start_ = Clock::now();
        for (auto& p : progress_) {
            p.acknowledged.store(0, std::memory_order_relaxed);
            p.retired.store(0, std::memory_order_relaxed);
            p.current.store(-1, std::memory_order_relaxed);
            p.completed.store(0, std::memory_order_relaxed);
        }
        for (auto& t : track_progress_) {
            t.started.store(0, std::memory_order_relaxed);
            t.finished.store(0, std::memory_order_relaxed);
            t.cpu.store(0, std::memory_order_relaxed);
            t.participant.store(-1, std::memory_order_relaxed);
            t.status.store(0, std::memory_order_relaxed);
        }
        jobs_ = jobs;
        job_count_ = count;
        capture_timing_ = captureTiming;
        next_job_.store(0, std::memory_order_relaxed);
        admission_.begin();
        if (count == 0)
            admission_.close();
    }

    // A plugin invocation itself cannot be preempted. Stop claiming new work
    // after the deadline; workers retain responsibility for remaining jobs.
    void participate(Clock::time_point deadline) {
        progress_[0].acknowledged.store(elapsedNs(), std::memory_order_release);
        while (Clock::now() < deadline && processOne(0)) {}
        retire(0);
    }

    bool completeBefore(Clock::time_point deadline) const {
        // Time-bounded polling: no mutex or OS wait on the callback. A late
        // batch remains owned until busy() becomes false.
        while (busy()) {
            if (Clock::now() >= deadline)
                return false;
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        return true;
    }

    // Control/offline thread only. Never called by realtime processAudio().
    void waitUntilIdle() const {
        while (busy())
            std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    // Called on the coordinator at fault, or the serialized control thread
    // while faulted. Never dereferences in-flight graph/context storage.
    AudioWorkerProgressSnapshot progressSnapshot() const {
        AudioWorkerProgressSnapshot snapshot;
        snapshot.track_count = job_count_;
        snapshot.pending_participants = pendingParticipants();
        for (size_t i = 0; i <= workers_.size(); ++i) {
            const auto& p = progress_[i];
            auto& out = snapshot.participants[i];
            out.retired_ns = p.retired.load(std::memory_order_acquire);
            out.acknowledged_ns = p.acknowledged.load(std::memory_order_acquire);
            out.current_track = p.current.load(std::memory_order_relaxed);
            out.completed_jobs = p.completed.load(std::memory_order_acquire);
            snapshot.completed_jobs += out.completed_jobs;
        }
        for (size_t i = 0; i < track_progress_.size() && i < job_count_; ++i) {
            const auto& t = track_progress_[i];
            auto& out = snapshot.tracks[i];
            out.finished_ns = t.finished.load(std::memory_order_acquire);
            out.cpu_ns = t.cpu.load(std::memory_order_relaxed);
            out.started_ns = t.started.load(std::memory_order_acquire);
            out.participant = t.participant.load(std::memory_order_relaxed);
            out.status = t.status.load(std::memory_order_relaxed);
        }
        return snapshot;
    }

private:
    static uint64_t threadCpuNs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec value{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
            return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
#endif
        return 0;
    }
    uint64_t elapsedNs() const {
        return 1 + static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - batch_start_).count());
    }
    bool processOne(uint32_t participant) {
        const auto index = next_job_.fetch_add(1, std::memory_order_relaxed);
        if (index >= job_count_)
            return false;
        // Every real job is now claimed. Exclude any worker that has not joined
        // yet; already-admitted workers still protect all reads and writes until
        // they retire, even if one was preempted before claiming a job.
        if (index + 1 == job_count_)
            admission_.close();
        auto& job = jobs_[index];
        progress_[participant].current.store(static_cast<int32_t>(index), std::memory_order_relaxed);
        if (index < track_progress_.size()) {
            track_progress_[index].participant.store(participant, std::memory_order_relaxed);
            track_progress_[index].started.store(elapsedNs(), std::memory_order_release);
        }
        const auto begin = capture_timing_ ? Clock::now() : Clock::time_point{};
        const auto cpuBegin = index < track_progress_.size() ? threadCpuNs() : 0;
        try {
            remidy::AudioThreadScope audioThreadScope;
            if (job.graph)
                job.status = job.graph->processAudio(*job.context);
        } catch (...) {
            job.status = -1;
        }
        const auto cpuEnd = cpuBegin ? threadCpuNs() : 0;
        if (capture_timing_)
            job.duration_nanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
        if (index < track_progress_.size()) {
            track_progress_[index].cpu.store(cpuEnd >= cpuBegin ? cpuEnd - cpuBegin : 0,
                                             std::memory_order_relaxed);
            track_progress_[index].status.store(job.status, std::memory_order_relaxed);
            track_progress_[index].finished.store(elapsedNs(), std::memory_order_release);
        }
        progress_[participant].completed.fetch_add(1, std::memory_order_release);
        progress_[participant].current.store(-1, std::memory_order_relaxed);
        return true;
    }

    void retire(uint32_t participant) {
        progress_[participant].retired.store(elapsedNs(), std::memory_order_release);
        // The RMW chain gathers every participant's writes. The final acquire
        // load of zero allows the coordinator to consume or destroy all jobs.
        admission_.leave();
    }

    void runWorker(uint32_t participant) {
        std::shared_ptr<void> threadContext;
        try {
            if (thread_setup_)
                threadContext = thread_setup_();
        } catch (...) {
            setup_failed_.store(true, std::memory_order_relaxed);
            started_.fetch_add(1, std::memory_order_release);
            return;
        }
        started_.fetch_add(1, std::memory_order_release);
        while (!stopping_.load(std::memory_order_acquire)) {
            const auto ticket = admission_.ticket();
            if (!AudioBatchAdmission::open(ticket)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            // A failed CAS means admission closed, ownership changed, or a newer
            // batch was published. Retry from the atomic; do not touch batch data.
            if (!admission_.tryEnter(ticket))
                continue;
            progress_[participant].acknowledged.store(elapsedNs(), std::memory_order_release);
            while (processOne(participant)) {}
            retire(participant);
            // No accesses to batch storage are allowed after releasing ownership.
        }
    }

    void stop() {
        // Non-RT destruction; also used to unwind partial thread creation.
        stopping_.store(true, std::memory_order_release);
        for (auto& worker : workers_)
            if (worker.joinable())
                worker.join();
    }

    struct ParticipantProgress {
        std::atomic<uint64_t> acknowledged{0}, retired{0};
        std::atomic<int32_t> current{-1};
        std::atomic<uint32_t> completed{0};
    };
    struct TrackProgress {
        std::atomic<uint64_t> started{0}, finished{0}, cpu{0};
        std::atomic<int32_t> participant{-1}, status{0};
    };
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<int32_t>::is_always_lock_free);
    std::array<ParticipantProgress, 33> progress_{};
    std::array<TrackProgress, 128> track_progress_{};
    Clock::time_point batch_start_{};
    std::vector<std::thread> workers_;
    AudioWorkerThreadSetup thread_setup_;
    std::atomic<bool> setup_failed_{false};
    std::atomic<bool> stopping_{false};
    AudioBatchAdmission admission_;
    std::atomic<uint32_t> next_job_{0};
    std::atomic<uint32_t> started_{0};
    AudioTrackJob* jobs_{};
    uint32_t job_count_{};
    bool capture_timing_{};
};

} // namespace uapmd
