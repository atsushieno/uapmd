#include "PluginFormatCLAP.hpp"
#include "HostClasses.hpp"
#include <cstdint>
#undef min
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>

namespace remidy {
    void RemidyCLAPHost::requestRestart() noexcept {
        // FIXME: implement
    }

    void RemidyCLAPHost::requestProcess() noexcept {
        // FIXME: implement
    }

    void RemidyCLAPHost::requestCallback() noexcept {
        // FIXME: implement
    }

    bool RemidyCLAPHost::threadCheckIsMainThread() const noexcept {
        return EventLoop::runningOnMainThread();
    }

    bool RemidyCLAPHost::threadCheckIsAudioThread() const noexcept {
        // FIXME: implement
        return true;
    }

    bool RemidyCLAPHost::guiRequestShow() noexcept {
        return true;
    }

    bool RemidyCLAPHost::guiRequestHide() noexcept {
        return true;
    }

    bool RemidyCLAPHost::guiRequestResize(uint32_t width, uint32_t height) noexcept {
        auto* instance = attached_instance.load();
        if (!instance)
            return false;
        return instance->handleGuiResize(width, height);
    }

    void RemidyCLAPHost::guiClosed(bool wasDestroyed) noexcept {
        Logger::global()->logWarning("guiClosed() not implemented");
        (void) wasDestroyed;
    }

    void RemidyCLAPHost::attachInstance(PluginInstanceCLAP *instance) noexcept {
        attached_instance.store(instance);
    }

    void RemidyCLAPHost::detachInstance(PluginInstanceCLAP *instance) noexcept {
        (void) instance;
        attached_instance.store(nullptr);
    }

    RemidyCLAPHost::~RemidyCLAPHost() {
        // Stop and join any outstanding timer threads to avoid keeping process alive
        std::unordered_map<clap_id, std::unique_ptr<Timer>> timersCopy;
        {
            std::lock_guard<std::mutex> lock(timersMutex);
            timersCopy.swap(timers_);
        }
        for (auto &kv : timersCopy) {
            auto &t = kv.second;
            if (!t) continue;
            t->running.store(false);
            t->cv.notify_all();
            // On shutdown, don't risk blocking forever: detach if thread doesn't exit promptly
            if (t->worker.joinable()) t->worker.detach();
        }
    }

    bool RemidyCLAPHost::timerSupportRegisterTimer(uint32_t periodMs, clap_id *timerId) noexcept {
        if (!timerId || periodMs == 0)
            return false;
        auto id = nextTimerId.fetch_add(1);
        auto t = std::make_unique<Timer>();
        t->id = id;
        t->periodMs = periodMs;
        t->running.store(true);
        // Spawn a worker thread which periodically posts the timer callback to the UI thread
        t->worker = std::thread([this, tt = t.get()](){
            while (tt->running.load()) {
                std::unique_lock<std::mutex> lk(tt->cvMutex);
                // Wake early on shutdown
                tt->cv.wait_for(lk, std::chrono::milliseconds(tt->periodMs), [&]{ return !tt->running.load(); });
                if (!tt->running.load()) break;
                lk.unlock();
                auto* inst = attached_instance.load();
                if (inst) {
                    // Dispatch timer: enqueue work onto the main thread and return immediately
                    inst->dispatchTimer(tt->id);
                }
            }
        });
        {
            std::lock_guard<std::mutex> lock(timersMutex);
            timers_.emplace(id, std::move(t));
        }
        *timerId = id;
        return true;
    }

    bool RemidyCLAPHost::timerSupportUnregisterTimer(clap_id timerId) noexcept {
        std::unique_ptr<Timer> t;
        {
            std::lock_guard<std::mutex> lock(timersMutex);
            auto it = timers_.find(timerId);
            if (it == timers_.end())
                return false;
            t = std::move(it->second);
            timers_.erase(it);
        }
        if (t) {
            t->running.store(false);
            t->cv.notify_all();
            if (t->worker.joinable()) {
                // Avoid blocking the main/UI thread: detach instead of join if called from it
                if (EventLoop::runningOnMainThread())
                    t->worker.detach();
                else
                    t->worker.join();
            }
        }
        return true;
    }
}
