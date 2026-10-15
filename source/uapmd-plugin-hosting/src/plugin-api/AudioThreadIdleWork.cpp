#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace uapmd_plugin_hosting {

    namespace {
        struct IdleWorkEntry {
            AudioThreadIdleWork* work;
            uint64_t done_generation;
        };

        std::mutex idle_work_mutex;
        std::vector<IdleWorkEntry> idle_work_entries;
        std::atomic<uint64_t> idle_work_version{1};
        std::atomic<uint64_t> next_idle_generation{1};
    }

    void registerAudioThreadIdleWork(AudioThreadIdleWork* work) {
        std::lock_guard lock(idle_work_mutex);
        idle_work_entries.push_back({work, 0});
        idle_work_version.fetch_add(1, std::memory_order_release);
    }

    void unregisterAudioThreadIdleWork(AudioThreadIdleWork* work) {
        std::lock_guard lock(idle_work_mutex);
        std::erase_if(idle_work_entries, [work](const IdleWorkEntry& e) { return e.work == work; });
        idle_work_version.fetch_add(1, std::memory_order_release);
    }

    AudioThreadIdleRunner::AudioThreadIdleRunner()
        : generation_(next_idle_generation.fetch_add(1, std::memory_order_relaxed)) {
    }

    void AudioThreadIdleRunner::runPending() {
        const auto version = idle_work_version.load(std::memory_order_acquire);
        if (version == seen_version_ && !pending_)
            return;
        std::unique_lock lock(idle_work_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        seen_version_ = version;
        pending_ = false;
        bool ran = false;
        for (auto& entry : idle_work_entries) {
            if (entry.done_generation == generation_)
                continue;
            if (ran) {
                pending_ = true;
                break;
            }
            if (entry.work->runOnAudioThreadIdle())
                entry.done_generation = generation_;
            else
                pending_ = true;
            ran = true;
        }
    }
}
