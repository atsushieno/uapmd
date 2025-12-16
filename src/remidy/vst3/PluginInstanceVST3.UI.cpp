#include "PluginFormatVST3.hpp"
#include <priv/event-loop.hpp>
#if defined(__linux__) || defined(__unix__)
#include "EventLoopLinux.hpp"
#endif

namespace remidy {

    PluginInstanceVST3::UISupport::UISupport(PluginInstanceVST3* owner) : owner(owner) {
#if SMTG_OS_LINUX
        // Auto-detect Wayland vs X11 on Linux
        auto* loop = dynamic_cast<EventLoopLinux*>(getEventLoop());
        if (loop && loop->getDisplayServerType() == DisplayServerType::Wayland) {
            target_ui_string = kPlatformTypeWaylandSurfaceID;
        } else {
            target_ui_string = kPlatformTypeX11EmbedWindowID;
        }
#elif SMTG_OS_MACOS
        target_ui_string = kPlatformTypeNSView;
#else
        target_ui_string = kPlatformTypeHWND;
#endif
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
                if (view->isPlatformTypeSupported(target_ui_string) == kResultOk) {
                    // Register resize handler
                    if (host_resize_handler) {
                        owner->owner->getHost()->setResizeRequestHandler(view, host_resize_handler);
                    }

                    // Set up IPlugFrame - plugin may call resizeView() during this
                    auto frame = owner->owner->getHost()->getPlugFrame();
                    view->setFrame(frame);

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
                    // Platform not supported or invalid view
                    if (view)
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
                owner->owner->getHost()->setResizeRequestHandler(view, nullptr);
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
}
