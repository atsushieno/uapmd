#pragma once

#include <cstdint>

namespace uapmd_plugin_hosting {

    // Work that has to run on the audio processing thread, but never inside a render.
    class AudioThreadIdleWork {
    public:
        virtual ~AudioThreadIdleWork() = default;
        // Runs on the audio thread between renders; returns true once nothing is left to do on that thread.
        virtual bool runOnAudioThreadIdle() = 0;
    };

    // Called by the owner of the work, from any thread other than an audio thread.
    void registerAudioThreadIdleWork(AudioThreadIdleWork* work);
    // Waits if an audio thread is running the work right now; it never runs after this returns.
    void unregisterAudioThreadIdleWork(AudioThreadIdleWork* work);

    // Owned by an audio thread that has idle time between renders; everything registered runs again on each new runner.
    class AudioThreadIdleRunner {
        uint64_t generation_;
        uint64_t seen_version_{0};
        bool pending_{true};

    public:
        AudioThreadIdleRunner();
        // Runs at most one unfinished work item, and never blocks.
        void runPending();
    };
}
