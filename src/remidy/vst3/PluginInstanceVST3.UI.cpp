#include "PluginFormatVST3.hpp"
#include <priv/event-loop.hpp>

namespace remidy {

    PluginInstanceVST3::UISupport::UISupport(PluginInstanceVST3* owner) : owner(owner) {
    }

    bool PluginInstanceVST3::UISupport::create(bool isFloating) {
        (void) isFloating; // VST3 doesn't distinguish floating/embedded at create time

        if (created)
            return true;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            if (!owner->controller)
                return;

            view = owner->controller->createView("editor");
            if (view) {
                if (view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultOk) {
                    // Register a placeholder resize handler IMMEDIATELY so it's available during setFrame()
                    // This will be replaced when setResizeRequestHandler() is called later
                    owner->owner->getHost()->setResizeRequestHandler(view, [](uint32_t, uint32_t) {
                        return true;  // Accept the resize for now
                    });

                    // Set up IPlugFrame - plugin may call resizeView() during this
                    auto frame = owner->owner->getHost()->getPlugFrame();
                    view->setFrame(frame);

                    // Try to get scaling support (optional)
                    void* scale_ptr = nullptr;
                    if (view->queryInterface(IPlugViewContentScaleSupport::iid, &scale_ptr) == kResultOk && scale_ptr) {
                        scale_support = reinterpret_cast<IPlugViewContentScaleSupport*>(scale_ptr);
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

    bool PluginInstanceVST3::UISupport::attachToParent(void *parent) {
        if (!created || !view)
            return false;

        // If already attached, just return success - don't call attached() again
        if (attached)
            return true;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            success = view->attached(parent, kPlatformTypeX11EmbedWindowID) == kResultOk;
        });

        if (success)
            attached = true;

        return success;
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

    void PluginInstanceVST3::UISupport::setResizeRequestHandler(std::function<bool(uint32_t, uint32_t)> handler) {
        host_resize_handler = std::move(handler);
        // Also set it on the HostApplication so it can delegate resizeView() calls
        // Use the view pointer as the key to identify this specific plugin instance
        if (view) {
            owner->owner->getHost()->setResizeRequestHandler(view, host_resize_handler);
        }
    }
}
