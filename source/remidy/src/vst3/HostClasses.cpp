#include <algorithm>
#include <cerrno>

#include "remidy/remidy.hpp"
#include "HostClasses.hpp"
#if !SMTG_OS_WINDOWS
#include <sys/select.h>
#endif
#if defined(__linux__) || defined(__unix__)
#include <wayland-client-core.h>
#include "../EventLoopLinux.hpp"
#endif
#include <public.sdk/source/vst/utility/stringconvert.h>

// Define Wayland interface IIDs
#ifdef HAVE_WAYLAND
DEF_CLASS_IID (Steinberg::IWaylandHost)
DEF_CLASS_IID (Steinberg::IWaylandFrame)
#endif

#include <algorithm>

namespace remidy_vst3 {
    void logNoInterface(std::string label, const TUID _iid) {
        // FIXME: was there any TUID formatter?
        auto iidString = std::format("{:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x}",
            (uint8_t) _iid[0], (uint8_t) _iid[1], (uint8_t) _iid[2], (uint8_t) _iid[3], (uint8_t) _iid[4],(uint8_t) _iid[5], (uint8_t) _iid[6], (uint8_t) _iid[7],
            (uint8_t) _iid[8], (uint8_t) _iid[9], (uint8_t) _iid[10], (uint8_t) _iid[11], (uint8_t) _iid[12], (uint8_t) _iid[13], (uint8_t) _iid[14], (uint8_t) _iid[15]);
        remidy::Logger::global()->logWarning("%s invoked for missing interface: %s", label.data(), iidString.data());
    }

    std::string vst3StringToStdString(String128& src) {
        return Steinberg::Vst::StringConvert::convert(src);
    }

    const std::basic_string<char16_t> HostApplication::name16t{Steinberg::Vst::StringConvert::convert("remidy")};

    HostApplication::HostApplication(remidy::Logger* logger): logger(logger) {
        // Instantiate nested implementation classes
        support = new PlugInterfaceSupportImpl(this);
        run_loop = new RunLoopImpl(this);
#ifdef HAVE_WAYLAND
        wayland_host = new WaylandHostImpl(this);
#endif
    }

    HostApplication::~HostApplication() {
        // Clean up nested implementation classes
        if (support) support->release();
        if (run_loop) run_loop->release();
#ifdef HAVE_WAYLAND
        if (wayland_host) wayland_host->release();
#endif
    }

    void HostApplication::startProcessing() {
        parameter_changes.startProcessing();
    }

    void HostApplication::stopProcessing() {
        parameter_changes.stopProcessing();
    }

    tresult PLUGIN_API HostApplication::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IHostApplication)
        QUERY_INTERFACE(_iid, obj, IHostApplication::iid, IHostApplication)

        // Return nested interface implementations
        if (FUnknownPrivate::iidEqual(_iid, IPlugInterfaceSupport::iid)) {
            if (support) support->addRef();
            *obj = support;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IParameterChanges::iid)) {
            auto iface = parameter_changes.asInterface();
            iface->addRef();
            *obj = iface;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IRunLoop::iid)) {
            if (run_loop) run_loop->addRef();
            *obj = run_loop;
            return kResultOk;
        }
#ifdef HAVE_WAYLAND
        if (FUnknownPrivate::iidEqual(_iid, IWaylandHost::iid)) {
            if (wayland_host) wayland_host->addRef();
            *obj = wayland_host;
            return kResultOk;
        }
#endif
        logNoInterface("IHostApplication::queryInterface", _iid);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::getName(String128 name) {
        name16t.copy((char16_t*) name, name16t.length());
        name[name16t.length()] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::createInstance(TUID cid, TUID _iid, void** obj) {
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual(cid, IAttributeList::iid))
            *obj = new HostAttributeList();
        else if (FUnknownPrivate::iidEqual(cid, IMessage::iid))
            *obj = new HostMessage();
        else {
            logNoInterface("IHostApplication::createInstance", cid);
            return kNoInterface;
        }
        return kResultOk;
    }

    // VectorStream
    tresult PLUGIN_API VectorStream::read(void* buffer, int32 numBytes, int32* numBytesRead) {
        if (!buffer || numBytes < 0)
            return kInvalidArgument;
        auto remaining = static_cast<int32>(data.size() - offset);
        if (remaining <= 0 || numBytes == 0) {
            if (numBytesRead)
                *numBytesRead = 0;
            return kResultFalse;
        }
        auto toCopy = std::min(remaining, numBytes);
        memcpy(buffer, data.data() + offset, static_cast<size_t>(toCopy));
        offset += toCopy;
        if (numBytesRead)
            *numBytesRead = toCopy;
        return kResultOk;
    }

    tresult PLUGIN_API VectorStream::write(void* buffer, int32 numBytes, int32* numBytesWritten) {
        if (!buffer || numBytes < 0)
            return kInvalidArgument;
        auto required = offset + static_cast<int64>(numBytes);
        if (required > static_cast<int64>(data.size()))
            data.resize(static_cast<size_t>(required));
        memcpy(data.data() + offset, buffer, static_cast<size_t>(numBytes));
        offset += numBytes;
        if (numBytesWritten)
            *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API VectorStream::seek(int64 pos, int32 mode, int64* result) {
        int64 newOffset = offset;
        switch (mode) {
            case IBStream::kIBSeekSet:
                newOffset = pos;
                break;
            case IBStream::kIBSeekCur:
                newOffset = offset + pos;
                break;
            case IBStream::kIBSeekEnd:
                newOffset = static_cast<int64>(data.size()) + pos;
                break;
            default:
                return kInvalidArgument;
        }
        if (newOffset < 0 || newOffset > static_cast<int64>(data.size()))
            return kInvalidArgument;
        offset = newOffset;
        if (result)
            *result = offset;
        return kResultOk;
    }

    tresult PLUGIN_API VectorStream::tell(int64* pos) {
        if (pos)
            *pos = offset;
        return kResultOk;
    }

    // PlugInterfaceSupportImpl
    tresult PLUGIN_API HostApplication::PlugInterfaceSupportImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPlugInterfaceSupport)
        QUERY_INTERFACE(_iid, obj, IPlugInterfaceSupport::iid, IPlugInterfaceSupport)
        logNoInterface("IPlugInterfaceSupport::queryInterface", _iid);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::PlugInterfaceSupportImpl::isPlugInterfaceSupported(const TUID _iid) {
        // Check if we support the requested interface
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler2::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IUnitHandler::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IMessage::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IAttributeList::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IRunLoop::iid)) return kResultOk;
#ifdef HAVE_WAYLAND
        if (FUnknownPrivate::iidEqual(_iid, IWaylandHost::iid)) return kResultOk;
        if (FUnknownPrivate::iidEqual(_iid, IWaylandFrame::iid)) return kResultOk;
#endif
        return kResultFalse;
    }

    // RunLoopImpl
    tresult PLUGIN_API HostApplication::RunLoopImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IRunLoop)
        QUERY_INTERFACE(_iid, obj, IRunLoop::iid, IRunLoop)
        logNoInterface("IRunLoop::queryInterface", _iid);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::registerEventHandler(IEventHandler* handler, FileDescriptor fd) {
#if SMTG_OS_WINDOWS
        (void)handler;
        (void)fd;
        return kNotImplemented;
#else
        if (!handler)
            return kInvalidArgument;

        auto info = std::make_shared<EventHandlerInfo>();
        info->handler = handler;
        info->fd = fd;
        info->active.store(true);

        // The worker must never block on the main thread: unregistration (and stopAll())
        // joins it from the main thread, so a blocking dispatch would deadlock.
        // dispatch_pending throttles like the former blocking dispatch did: no new task
        // is enqueued until the previous one ran (select() is level-triggered, so a
        // still-readable fd re-fires on the next iteration).
        info->worker = std::thread([info]() {
            fd_set readfds;
            struct timeval tv;

            while (info->active.load()) {
                FD_ZERO(&readfds);
                FD_SET(info->fd, &readfds);

                tv.tv_sec = 0;
                tv.tv_usec = 100000;  // 100ms

                int result = select(info->fd + 1, &readfds, nullptr, nullptr, &tv);

                if (result < 0) {
                    if (errno == EINTR)
                        continue;
                    // EBADF etc.: plugins may close the fd before unregistering the
                    // handler; stop polling instead of spinning on the error.
                    break;
                }
                if (result > 0 && FD_ISSET(info->fd, &readfds)
                    && !info->dispatch_pending.exchange(true)) {
                    remidy::EventLoop::enqueueTaskOnMainThread([info]() {
                        if (info->active.load() && info->handler)
                            info->handler->onFDIsSet(info->fd);
                        info->dispatch_pending.store(false);
                    });
                }
            }
        });

        {
            std::lock_guard<std::mutex> lock(event_handlers_mutex);
            event_handlers.push_back(info);
        }
        return kResultOk;
#endif
    }

    namespace {
        template<typename Info>
        void stopWorker(Info& info) {
            info.active.store(false);
            if (info.worker.joinable()) {
                if (info.worker.get_id() == std::this_thread::get_id())
                    info.worker.detach();
                else
                    info.worker.join();
            }
        }
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::unregisterEventHandler(IEventHandler* handler) {
        if (!handler)
            return kInvalidArgument;

        std::shared_ptr<EventHandlerInfo> info;
        {
            std::lock_guard<std::mutex> lock(event_handlers_mutex);
            auto it = std::find_if(event_handlers.begin(), event_handlers.end(),
                                   [handler](auto& e) { return e->handler == handler; });
            if (it == event_handlers.end())
                return kInvalidArgument;
            info = *it;
            event_handlers.erase(it);
        }
        stopWorker(*info);
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::registerTimer(ITimerHandler* handler, TimerInterval milliseconds) {
        if (!handler)
            return kInvalidArgument;

        auto timer_info = std::make_shared<TimerInfo>();
        timer_info->handler = handler;
        timer_info->interval_ms = milliseconds;
        timer_info->active.store(true);

        // Same non-blocking dispatch contract as registerEventHandler(); the cv lets
        // unregisterTimer()/stopAll() interrupt the wait so join returns promptly.
        timer_info->worker = std::thread([timer_info]() {
            while (timer_info->active.load()) {
                {
                    std::unique_lock<std::mutex> lk(timer_info->cvMutex);
                    timer_info->cv.wait_for(lk, std::chrono::milliseconds(timer_info->interval_ms),
                                            [&] { return !timer_info->active.load(); });
                }
                if (!timer_info->active.load())
                    break;

                if (!timer_info->dispatch_pending.exchange(true)) {
                    remidy::EventLoop::enqueueTaskOnMainThread([timer_info]() {
                        if (timer_info->active.load() && timer_info->handler)
                            timer_info->handler->onTimer();
                        timer_info->dispatch_pending.store(false);
                    });
                }
            }
        });

        {
            std::lock_guard<std::mutex> lock(timers_mutex);
            timers.push_back(timer_info);
        }
        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::unregisterTimer(ITimerHandler* handler) {
        if (!handler)
            return kInvalidArgument;

        std::shared_ptr<TimerInfo> info;
        {
            std::lock_guard<std::mutex> lock(timers_mutex);
            auto it = std::find_if(timers.begin(), timers.end(),
                                   [handler](auto& t) { return t->handler == handler; });
            if (it == timers.end())
                return kInvalidArgument;
            info = *it;
            timers.erase(it);
        }
        info->active.store(false);
        info->cv.notify_all();
        stopWorker(*info);
        return kResultOk;
    }

    void HostApplication::RunLoopImpl::stopAll() {
        std::vector<std::shared_ptr<EventHandlerInfo>> handlersCopy;
        {
            std::lock_guard<std::mutex> lock(event_handlers_mutex);
            handlersCopy.swap(event_handlers);
        }
        for (auto& info : handlersCopy)
            stopWorker(*info);

        std::vector<std::shared_ptr<TimerInfo>> timersCopy;
        {
            std::lock_guard<std::mutex> lock(timers_mutex);
            timersCopy.swap(timers);
        }
        for (auto& info : timersCopy) {
            info->active.store(false);
            info->cv.notify_all();
            stopWorker(*info);
        }
    }

#ifdef HAVE_WAYLAND
    // WaylandHostImpl
    tresult PLUGIN_API HostApplication::WaylandHostImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IWaylandHost)
        QUERY_INTERFACE(_iid, obj, IWaylandHost::iid, IWaylandHost)
        logNoInterface("IWaylandHost::queryInterface", _iid);
        *obj = nullptr;
        return kNoInterface;
    }

    wl_display* PLUGIN_API HostApplication::WaylandHostImpl::openWaylandConnection() {
        const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
        return wl_display_connect(wayland_display);
    }

    tresult PLUGIN_API HostApplication::WaylandHostImpl::closeWaylandConnection(wl_display* display) {
        if (!display) {
            owner->logger->logError("WaylandHost::closeWaylandConnection: display pointer is null");
            return kResultFalse;
        }
        wl_display_disconnect(display);
        return kResultOk;
    }
#endif
}
