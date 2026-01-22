#include "HostClasses.hpp"
#include "PluginFormatVST3.hpp"
#include <priv/event-loop.hpp>
#include <thread>
#include <chrono>
#if !SMTG_OS_WINDOWS
#include <sys/select.h>
#endif
#if defined(__linux__) || defined(__unix__)
#include "../EventLoopLinux.hpp"
#endif

namespace remidy {

    PluginInstanceVST3::UISupport::UISupport(PluginInstanceVST3* owner) : owner(owner) {
#if SMTG_OS_LINUX
        // On Linux, we support both Wayland and X11.
        // Prioritize based on current session type, but try the other as fallback.
        // This allows X11 plugins to work on Wayland via XWayland/Wayback.
        const char* xdgSessionType = std::getenv("XDG_SESSION_TYPE");
        const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
        bool preferWayland = xdgSessionType && !strcmp(xdgSessionType, "wayland") &&
                             wayland_display && wayland_display[0] != '\0';
        if (preferWayland) {
            supported_ui_types = {kPlatformTypeWaylandSurfaceID, kPlatformTypeX11EmbedWindowID};
        } else {
            supported_ui_types = {kPlatformTypeX11EmbedWindowID, kPlatformTypeWaylandSurfaceID};
        }
#elif SMTG_OS_MACOS
        supported_ui_types = {kPlatformTypeNSView};
#else
        supported_ui_types = {kPlatformTypeHWND};
#endif
        plug_frame = new PlugFrameImpl(this);
    }

    PluginInstanceVST3::UISupport::~UISupport() {
        if (plug_frame) plug_frame->release();
    }

    bool PluginInstanceVST3::UISupport::hasUI() {
        // FIXME: at this state we cannot reliably check whether the plugin has a UI or not...
        return owner->controller != nullptr;
    }

    bool PluginInstanceVST3::UISupport::create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
        if (created)
            return false; // Already created

        host_resize_handler = resizeHandler;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            if (!owner->controller)
                return;

            view = owner->controller->createView("editor");
            if (view) {
                // Find a supported UI type from our list of candidates.
                // This allows fallback (e.g., X11 on Wayland via XWayland).
                target_ui_string = nullptr;
                for (auto& uiType : supported_ui_types) {
                    if (view->isPlatformTypeSupported(uiType) == kResultOk) {
                        target_ui_string = uiType;
                        Logger::global()->logInfo("Instantiated Plugin UI for %s as %s", owner->info()->displayName().data(), uiType);
                        break;
                    }
                }

                if (target_ui_string) {
                    // Register resize handler
                    if (host_resize_handler)
                        plug_frame->resize_request_handler = host_resize_handler;

                    // Set up IPlugFrame - plugin may call resizeView() during this
                    view->setFrame(plug_frame);

                    // Try to get scaling support (optional)
                    void* scale_ptr = nullptr;
                    if (view->queryInterface(IPlugViewContentScaleSupport::iid, &scale_ptr) == kResultOk && scale_ptr) {
                        scale_support = reinterpret_cast<IPlugViewContentScaleSupport*>(scale_ptr);
                    }

                    // Attach to parent if embedded
                    if (!isFloating && parentHandle) {
                        if (view->attached(parentHandle, target_ui_string) == kResultOk) {
                            attached = true;

                            // Notify host of initial UI size
                            if (host_resize_handler) {
                                ViewRect rect{};
                                if (view->getSize(&rect) == kResultOk) {
                                    uint32_t width = rect.right - rect.left;
                                    uint32_t height = rect.bottom - rect.top;
                                    if (width > 0 && height > 0) {
                                        host_resize_handler(width, height);
                                    }
                                }
                            }
                        } else {
                            // Failed to attach
                            view->release();
                            view = nullptr;
                            if (scale_support) {
                                scale_support->release();
                                scale_support = nullptr;
                            }
                            return;
                        }
                    }

                    created = true;
                    success = true;
                } else {
                    // No supported platform type found
                    view->release();
                    view = nullptr;
                }
            }
        });

        return success;
    }

    void PluginInstanceVST3::UISupport::destroy() {
        if (!created)
            return;

        EventLoop::runTaskOnMainThread([&] {
            if (scale_support) {
                scale_support->release();
                scale_support = nullptr;
            }

            if (view) {
                // Unregister the resize handler before destroying the view
                plug_frame->resize_request_handler = nullptr;
                view->removed();
                view->release();
                view = nullptr;
            }

            created = false;
            visible = false;
            attached = false;
        });
    }

    bool PluginInstanceVST3::UISupport::show() {
        if (!created)
            return false;

        // VST3 has no show() API - we just track state
        visible = true;
        return true;
    }

    void PluginInstanceVST3::UISupport::hide() {
        if (!created)
            return;

        // VST3 has no hide() API - we just track state
        visible = false;
    }

    void PluginInstanceVST3::UISupport::setWindowTitle(std::string title) {
        (void) title;
        // VST3 IPlugView has no setWindowTitle API - host's responsibility
    }

    bool PluginInstanceVST3::UISupport::canResize() {
        if (!created || !view)
            return false;

        bool can_resize = false;
        EventLoop::runTaskOnMainThread([&] {
            can_resize = view->canResize() == kResultTrue;
        });

        return can_resize;
    }

    bool PluginInstanceVST3::UISupport::getSize(uint32_t &width, uint32_t &height) {
        if (!created || !view)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            ViewRect rect{};
            if (view->getSize(&rect) == kResultOk) {
                width = rect.right - rect.left;
                height = rect.bottom - rect.top;
                success = true;
            }
        });

        return success;
    }

    bool PluginInstanceVST3::UISupport::setSize(uint32_t width, uint32_t height) {
        if (!created || !view)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            ViewRect rect{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
            auto result = view->onSize(&rect);
            success = result == kResultOk;
        });

        return success;
    }

    bool PluginInstanceVST3::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        if (!created || !view)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            ViewRect rect{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
            if (view->checkSizeConstraint(&rect) == kResultOk) {
                width = rect.right - rect.left;
                height = rect.bottom - rect.top;
                success = true;
            }
        });

        return success;
    }

    bool PluginInstanceVST3::UISupport::setScale(double scale) {
        if (!created || !scale_support)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            success = scale_support->setContentScaleFactor(scale) == kResultOk;
        });

        return success;
    }

    // PlugFrameImpl
    tresult PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPlugFrame)
        QUERY_INTERFACE(_iid, obj, IPlugFrame::iid, IPlugFrame)
#ifdef HAVE_WAYLAND
        QUERY_INTERFACE(_iid, obj, IWaylandFrame::iid, IWaylandFrame)
#endif
        // Return IRunLoop implementation
        if (FUnknownPrivate::iidEqual(_iid, IRunLoop::iid)) {
            if (run_loop) run_loop->addRef();
            *obj = run_loop;
            return kResultOk;
        }
        logNoInterface("IPlugFrame::queryInterface", _iid);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::resizeView(IPlugView* view, ViewRect* newSize) {
        if (!view || !newSize)
            return kInvalidArgument;

        // Check if there's a resize handler registered for this view
        uint32_t width = newSize->right - newSize->left;
        uint32_t height = newSize->bottom - newSize->top;
        if (resize_request_handler && resize_request_handler(width, height))
            return kResultOk;

        return kResultFalse;
    }

#if HAVE_WAYLAND
    // PlugFrameImpl - IWaylandFrame methods
    wl_surface* PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::getWaylandSurface(wl_display* display) {
        // Return the parent surface that the plugin should use as parent
        // For now, we don't create a surface - the plugin creates its own top-level surface
        // In a real implementation, you'd create and return a wl_surface here
        (void)display;
        return parent_surface;
    }

    xdg_surface* PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::getParentSurface(ViewRect& parentSize, wl_display* display) {
        // Return the parent XDG surface for the plugin to attach to
        // parentSize would be filled with the parent window dimensions
        // For now, return what we have (likely nullptr in headless mode)
        (void)parentSize;
        (void)display;
        return parent_xdg_surface;
    }

    xdg_toplevel* PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::getParentToplevel(wl_display* display) {
        // Return the parent XDG toplevel for dialogs/transient windows
        (void)display;
        return parent_xdg_toplevel;
    }
#endif

    // PlugFrameImpl::RunLoopImpl
    tresult PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::RunLoopImpl::queryInterface(const TUID _iid, void** obj) {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IRunLoop)
        QUERY_INTERFACE(_iid, obj, IRunLoop::iid, IRunLoop)
        logNoInterface("IRunLoop::queryInterface", _iid);
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::RunLoopImpl::registerEventHandler(IEventHandler* handler, FileDescriptor fd) {
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

        auto info = std::make_shared<PlugFrameImpl::EventHandlerInfo>();
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
                        if (handler)
                            handler->onFDIsSet(info->fd);
                    });
                }
            }
        });
        monitor_thread.detach();

        return kResultOk;
#endif
    }

    tresult PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::RunLoopImpl::unregisterEventHandler(IEventHandler* handler) {
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

    tresult PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::RunLoopImpl::registerTimer(ITimerHandler* handler, TimerInterval milliseconds) {
        if (!handler)
            return kInvalidArgument;

        std::lock_guard<std::mutex> lock(owner->timers_mutex);

        auto timer_info = std::make_shared<PlugFrameImpl::TimerInfo>();
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
                    if (handler)
                        handler->onTimer();
                });
            }
        });
        timer_thread.detach();

        return kResultOk;
    }

    tresult PLUGIN_API PluginInstanceVST3::UISupport::PlugFrameImpl::RunLoopImpl::unregisterTimer(ITimerHandler* handler) {
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
}
