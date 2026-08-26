#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <remidy/remidy.hpp>

// Handing a finished analysis back to the main thread.
//
// Every backend here analyses on a worker thread, but the timeline facade
// mutations it finishes with are main-thread work: each one republishes the
// real-time snapshot through RtSnapshotPublisher, whose publish() is documented
// as publishing-thread only and which retires -- and eventually frees -- the
// very snapshot object the UI thread is reading, and each one that carries a
// User origin records a step into the undo history the UI owns. Doing either
// from a worker races the UI thread. So the analysis stays on the worker and
// only the edit comes back.
//
// An edit can still be waiting for the event loop when the job that queued it
// is torn down, so every edit carries a weak reference to a token the job owns.
// Once the job drops the token, a queued edit is discarded instead of reaching
// into an engine that is on its way out.
//
// Compiled into each backend rather than into a library of its own, for the
// same reason as ClipTranscription: the backends end up in separate binaries.
namespace uapmd_mir {

struct AsyncEditLifetime {};

using AsyncEditLifetimeRef = std::shared_ptr<AsyncEditLifetime>;

inline AsyncEditLifetimeRef makeAsyncEditLifetime() {
    return std::make_shared<AsyncEditLifetime>();
}

// Runs `edit` on the main thread: immediately when the caller already is the
// main thread, otherwise on the next turn of the event loop. A revoked (null)
// token drops the edit, which is what a caller that is shutting down wants.
inline void runTimelineEditOnMainThread(
        const AsyncEditLifetimeRef& lifetime, std::function<void()> edit) {
    if (!lifetime || !edit)
        return;
    if (remidy::EventLoop::runningOnMainThread()) {
        edit();
        return;
    }
    remidy::EventLoop::enqueueTaskOnMainThread(
        [weak = std::weak_ptr<AsyncEditLifetime>(lifetime), edit = std::move(edit)] {
            if (weak.lock())
                edit();
        });
}

} // namespace uapmd_mir
