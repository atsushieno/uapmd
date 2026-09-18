#include "JsfxGfxScheduler.hpp"

#include <algorithm>

namespace uapmd_jsfx {

    namespace {
        // Two is the smallest pool that keeps other editors drawing while one waits on a
        // menu. The web build's worker threads are a scarce, fixed resource, so it gets the
        // minimum; desktop can afford enough for a few editors to redraw in parallel.
#if defined(__EMSCRIPTEN__)
        constexpr uint32_t kDefaultWorkers = 2;
#else
        constexpr uint32_t kDefaultWorkers = 4;
#endif
        constexpr auto kIdleWait = std::chrono::milliseconds{50};
    }

    JsfxGfxScheduler::JsfxGfxScheduler(uint32_t workerCount) {
        if (workerCount < 1)
            workerCount = 1;
        workers_.reserve(workerCount);
        for (uint32_t i = 0; i < workerCount; i++)
            workers_.emplace_back([this] { run(); });
    }

    JsfxGfxScheduler::~JsfxGfxScheduler() {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable())
                worker.join();
    }

    JsfxGfxScheduler& JsfxGfxScheduler::shared() {
        static JsfxGfxScheduler instance{kDefaultWorkers};
        return instance;
    }

    void JsfxGfxScheduler::add(GfxTickable* target) {
        if (!target)
            return;
        {
            std::lock_guard lock{mutex_};
            for (auto& entry : entries_)
                if (entry.target == target)
                    return;
            entries_.emplace_back(Entry{next_id_++, target, std::chrono::steady_clock::now(), false});
        }
        condition_.notify_one();
    }

    void JsfxGfxScheduler::remove(GfxTickable* target) {
        std::unique_lock lock{mutex_};
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [target](const Entry& e) { return e.target == target; });
        if (it == entries_.end())
            return;
        // Take it out of the rotation first, so no worker picks it up again, then wait for
        // whichever worker has it now. The caller is responsible for having released
        // anything that tick might be waiting on.
        const uint64_t id = it->id;
        it->target = nullptr;
        condition_.wait(lock, [this, id] {
            return std::none_of(entries_.begin(), entries_.end(),
                                [id](const Entry& e) { return e.id == id && e.running; });
        });
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [id](const Entry& e) { return e.id == id; }),
                       entries_.end());
    }

    void JsfxGfxScheduler::run() {
        std::unique_lock lock{mutex_};
        while (!stopping_) {
            const auto now = std::chrono::steady_clock::now();

            // Pick the instance that has been waiting longest. Taking it out of
            // consideration while it runs is what keeps a slow script from being ticked
            // again before it has finished the previous frame; no separate backpressure
            // counter is needed.
            auto chosen = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (!it->target || it->running || it->due > now)
                    continue;
                if (chosen == entries_.end() || it->due < chosen->due)
                    chosen = it;
            }

            if (chosen == entries_.end()) {
                // Sleep until the earliest thing that is not already running comes due.
                auto wake = now + kIdleWait;
                for (auto& entry : entries_)
                    if (entry.target && !entry.running && entry.due < wake)
                        wake = entry.due;
                condition_.wait_until(lock, wake);
                continue;
            }

            auto* target = chosen->target;
            const uint64_t id = chosen->id;
            chosen->running = true;
            lock.unlock();

            target->tickGfx();
            uint32_t rate = target->desiredFrameRate();
            if (rate < 1)
                rate = 1;
            if (rate > 120)
                rate = 120;
            const auto interval = std::chrono::milliseconds{1000 / rate};

            lock.lock();
            // Found by id rather than by pointer: the entry may have been removed while it
            // was running, in which case its target is already cleared and remove() is
            // waiting for exactly this.
            for (auto& entry : entries_) {
                if (entry.id != id)
                    continue;
                entry.running = false;
                entry.due = std::chrono::steady_clock::now() + interval;
                break;
            }
            condition_.notify_all();
        }
    }

}
