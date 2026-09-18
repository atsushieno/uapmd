#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace uapmd_jsfx {

    // Something the scheduler drives at a frame rate of its own choosing.
    class GfxTickable {
    public:
        virtual ~GfxTickable() = default;
        // Draws one frame. It may block -- a script that opens a menu waits here for the
        // user -- which is why the scheduler runs several workers rather than one.
        virtual void tickGfx() = 0;
        // Frames per second this instance would like. Read afresh each time, because a
        // script may change it.
        virtual uint32_t desiredFrameRate() = 0;
    };

    // Drives every visible JSFX editor from a small pool of threads.
    //
    // One thread per editor does not work here: the Emscripten build has a fixed pool of
    // worker threads and the engine already spends several of them, so a project with a
    // handful of JSFX effects would exhaust it. A fixed pool with a queue of instances that
    // are due costs the same however many editors are open.
    //
    // A script that opens a menu blocks the worker that is drawing it until the user
    // chooses. That costs one worker and nothing else -- each instance has its own ysfx
    // state and its own lock, so the others keep drawing. Pool size is therefore a
    // smoothness dial rather than a correctness one.
    class JsfxGfxScheduler {
        struct Entry {
            uint64_t id;
            GfxTickable* target;
            std::chrono::steady_clock::time_point due;
            bool running;
        };

        std::vector<std::thread> workers_{};
        std::vector<Entry> entries_{};
        std::mutex mutex_{};
        std::condition_variable condition_{};
        uint64_t next_id_{1};
        bool stopping_{false};

        void run();

    public:
        explicit JsfxGfxScheduler(uint32_t workerCount);
        ~JsfxGfxScheduler();

        // The scheduler is shared by every JSFX instance in the process. It starts its
        // threads the first time an editor is added and keeps them for the lifetime of the
        // process, because editors open and close constantly.
        static JsfxGfxScheduler& shared();

        void add(GfxTickable* target);
        // Removes `target` and does not return until it is no longer being ticked, so the
        // caller can destroy it afterwards. A worker blocked inside the target's tick must
        // be released by the caller first, or this waits for the user.
        void remove(GfxTickable* target);
    };

}
