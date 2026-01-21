#include <thread>
#include <algorithm>

#include "HostClasses.hpp"
#if !SMTG_OS_WINDOWS
#include <sys/select.h>
#endif
#include <priv/event-loop.hpp>
#if defined(__linux__) || defined(__unix__)
#include <wayland-client-core.h>
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
        event_handler = new EventHandlerImpl(this);
        support = new PlugInterfaceSupportImpl(this);
        run_loop = new RunLoopImpl(this);
#ifdef HAVE_WAYLAND
        wayland_host = new WaylandHostImpl(this);
#endif
    }

    HostApplication::~HostApplication() {
        // Clean up nested implementation classes
        if (event_handler) event_handler->release();
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
        if (FUnknownPrivate::iidEqual(_iid, IRunLoop::iid)) {
            if (run_loop) run_loop->addRef();
            *obj = run_loop;
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IParameterChanges::iid)) {
            auto iface = parameter_changes.asInterface();
            iface->addRef();
            *obj = iface;
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

    // EventHandlerImpl
    tresult PLUGIN_API HostApplication::EventHandlerImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IEventHandler)
        QUERY_INTERFACE(_iid, obj, IEventHandler::iid, IEventHandler)
        logNoInterface("IEventHandler::queryInterface", _iid);
        *obj = nullptr;
        return kNoInterface;
    }

    void PLUGIN_API HostApplication::EventHandlerImpl::onFDIsSet(int fd) {
        // Event handler implementation - currently not used
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
        // File descriptor event handling not supported on Windows
        // Windows plugins should use timers instead
        (void)handler;
        (void)fd;
        return kNotImplemented;
#else
        if (!handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->event_handlers_mutex);

        auto info = std::make_shared<HostApplication::EventHandlerInfo>();
        info->handler = handler;
        info->fd = fd;
        info->active.store(true);

        owner->event_handlers.push_back(info);

        // Start a background thread to monitor this file descriptor
        std::thread monitor_thread([info]() {
            fd_set readfds;
            struct timeval tv;

            while (info->active.load()) {
                FD_ZERO(&readfds);
                FD_SET(info->fd, &readfds);

                // Timeout for select so we can check active flag periodically
                tv.tv_sec = 0;
                tv.tv_usec = 100000;  // 100ms

                int result = select(info->fd + 1, &readfds, nullptr, nullptr, &tv);

                if (result > 0 && FD_ISSET(info->fd, &readfds)) {
                    // File descriptor has data available
                    remidy::EventLoop::runTaskOnMainThread([info]() {
                        if (!info->active.load()) return;

                        auto handler = info->handler;
                        if (handler) {
                            handler->onFDIsSet(info->fd);
                        }
                    });
                }
            }
        });
        monitor_thread.detach();

        return kResultOk;
#endif
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::unregisterEventHandler(IEventHandler* handler) {
        if (!handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->event_handlers_mutex);
        
        auto it = owner->event_handlers.begin();
        while (it != owner->event_handlers.end()) {
            if ((*it)->handler == handler) {
                (*it)->active.store(false);
                it = owner->event_handlers.erase(it);
                return kResultOk;
            } else {
                ++it;
            }
        }

        return kInvalidArgument;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::registerTimer(ITimerHandler* handler, TimerInterval milliseconds) {
        if (!handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->timers_mutex);
        
        auto timer_info = std::make_shared<HostApplication::TimerInfo>();
        timer_info->handler = handler;
        timer_info->interval_ms = milliseconds;
        timer_info->active.store(true);
        
        owner->timers.push_back(timer_info);

        // Start timer thread
        std::thread timer_thread([timer_info]() {
            while (timer_info->active.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(timer_info->interval_ms));
                
                remidy::EventLoop::runTaskOnMainThread([timer_info]() {
                    if (!timer_info->active.load()) return;

                    auto handler = timer_info->handler;
                    if (handler) {
                        handler->onTimer();
                    }
                });
            }
        });
        timer_thread.detach();

        return kResultOk;
    }

    tresult PLUGIN_API HostApplication::RunLoopImpl::unregisterTimer(ITimerHandler* handler) {
        if (!handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->timers_mutex);
        
        auto it = owner->timers.begin();
        while (it != owner->timers.end()) {
            if ((*it)->handler == handler) {
                (*it)->active.store(false);
                it = owner->timers.erase(it);
                return kResultOk;
            } else {
                ++it;
            }
        }

        return kInvalidArgument;
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
