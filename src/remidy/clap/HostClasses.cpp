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
        Logger::global()->logInfo("RemidyCLAPHost::requestRestart() is not implemented");
    }

    void RemidyCLAPHost::requestProcess() noexcept {
        // FIXME: implement
        Logger::global()->logInfo("RemidyCLAPHost::requestProcess() is not implemented");
    }

    void RemidyCLAPHost::requestCallback() noexcept {
        // FIXME: implement
        Logger::global()->logInfo("RemidyCLAPHost::requestCallback() is not implemented");
    }

    void RemidyCLAPHost::paramsRescan(clap_param_rescan_flags flags) noexcept {
        auto* instance = attached_instance.load();
        if (!instance)
            return;

        // Parameter list has changed, need to rescan
        auto* params = dynamic_cast<PluginInstanceCLAP::ParameterSupport*>(instance->_parameters);
        if (!params)
            return;

        if (flags & CLAP_PARAM_RESCAN_VALUES) {
            // Parameter values changed - refresh all parameter values
            //Logger::global()->logInfo("CLAP: Parameter values changed, refreshing");
            auto& paramDefs = params->parameters();
            for (size_t i = 0; i < paramDefs.size(); ++i) {
                double value = 0.0;
                if (params->getParameter(static_cast<uint32_t>(i), &value) == StatusCode::OK) {
                    auto paramId = params->getParameterId(static_cast<uint32_t>(i));
                    params->notifyParameterValue(paramId, value);
                }
            }
        }
        if (flags & CLAP_PARAM_RESCAN_INFO) {
            // Parameter info (names, ranges) changed - refresh metadata
            //Logger::global()->logInfo("CLAP: Parameter info changed, refreshing metadata");
            auto& paramDefs = params->parameters();
            for (size_t i = 0; i < paramDefs.size(); ++i) {
                params->refreshParameterMetadata(static_cast<uint32_t>(i));
            }
        }
        if (flags & CLAP_PARAM_RESCAN_TEXT) {
            // Parameter text representations changed
            Logger::global()->logInfo("CLAP: Parameter text changed");
            // Text changes don't require action - they're fetched on demand via valueToString
        }
        if (flags & CLAP_PARAM_RESCAN_ALL) {
            // Full rescan needed - parameter list structure has changed
            //Logger::global()->logWarning("CLAP: Full parameter rescan requested - this requires rebuilding the parameter list");
            params->refreshAllParameterMetadata();
        }
    }

    void RemidyCLAPHost::paramsClear(clap_id paramId, clap_param_clear_flags flags) noexcept {
        // FIXME: implement
        Logger::global()->logInfo("RemidyCLAPHost::paramsClear() is not implemented");
    }

    void RemidyCLAPHost::paramsRequestFlush() noexcept {
        auto* instance = attached_instance.load();
        if (!instance)
            return;

        // CLAP spec: This method must not be called on the audio thread
        // If called from audio thread, we should not process it
        if (threadCheckIsAudioThread()) {
            Logger::global()->logWarning("paramsRequestFlush() called from audio thread, ignoring");
            return;
        }

        // Plugin is requesting a parameter flush
        // We need to call plugin->paramsFlush() with input/output event lists
        auto flushParams = [instance]() {
            if (!instance->plugin || !instance->plugin->canUseParams())
                return;

            // Use the instance's event list for both input and output
            instance->events->clear();
            instance->plugin->paramsFlush(
                instance->events->clapInputEvents(),
                instance->events->clapOutputEvents()
            );

            // Process any output events from the flush
            size_t eventCount = instance->events->size();
            for (size_t i = 0; i < eventCount; ++i) {
                auto* hdr = instance->events->get(static_cast<uint32_t>(i));

                if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
                    continue;

                if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
                    auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
                    auto* params = dynamic_cast<PluginInstanceCLAP::ParameterSupport*>(instance->_parameters);
                    if (params)
                        params->notifyParameterValue(ev->param_id, ev->value);
                }
            }

            instance->events->clear();
        };

        // If we're already on the main thread, execute immediately
        // Otherwise, schedule on main thread
        if (threadCheckIsMainThread()) {
            flushParams();
        } else {
            EventLoop::runTaskOnMainThread(flushParams);
        }
    }

    bool RemidyCLAPHost::threadCheckIsMainThread() const noexcept {
        return EventLoop::runningOnMainThread();
    }

    bool RemidyCLAPHost::threadCheckIsAudioThread() const noexcept {
        auto thisId = std::this_thread::get_id();
        return std::ranges::any_of(audioThreadIds(), [&](auto tid) { return tid == thisId; });
    }

    bool RemidyCLAPHost::guiRequestShow() noexcept {
        auto i = attached_instance.load();
        if (!i)
            return false;
        bool result;
        EventLoop::runTaskOnMainThread([&] { result = i->ui()->show(); });
        return result;
    }

    bool RemidyCLAPHost::guiRequestHide() noexcept {
        auto i = attached_instance.load();
        if (!i)
            return false;
        EventLoop::runTaskOnMainThread([&] { i->ui()->hide(); });
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
