#include "PluginFormatVST3.hpp"
#include <priv/event-loop.hpp>
#include <travesty/view.h>

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

            auto result = owner->controller->vtable->controller.create_view(owner->controller, "editor");
            if (result) {
                view = reinterpret_cast<IPlugView*>(result);
                if (view && view->vtable->view.is_platform_type_supported(view, V3_VIEW_PLATFORM_TYPE_NATIVE) == V3_OK) {
                    // Set up IPlugFrame
                    view->vtable->view.set_frame(view, reinterpret_cast<v3_plugin_frame**>(owner->owner->getHost()->getPlugFrame()));

                    // Try to get scaling support (optional)
                    void* scale_ptr = nullptr;
                    if (view->vtable->unknown.query_interface(view, v3_plugin_view_content_scale_iid, &scale_ptr) == V3_OK && scale_ptr) {
                        scale_support = reinterpret_cast<IPlugViewContentScaleSupport*>(scale_ptr);
                    }

                    created = true;
                    success = true;

                    // Now that the view is created, register any pending resize handler
                    if (host_resize_handler) {
                        owner->owner->getHost()->setResizeRequestHandler(view, host_resize_handler);
                    }
                } else {
                    // Platform not supported or invalid view
                    if (view)
                        view->vtable->unknown.unref(view);
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
                scale_support->vtable->unknown.unref(scale_support);
                scale_support = nullptr;
            }

            if (view) {
                // Unregister the resize handler before destroying the view
                owner->owner->getHost()->setResizeRequestHandler(view, nullptr);
                view->vtable->view.removed(view);
                view->vtable->unknown.unref(view);
                view = nullptr;
            }

            created = false;
            visible = false;
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

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            success = view->vtable->view.attached(view, parent, V3_VIEW_PLATFORM_TYPE_NATIVE) == V3_OK;
        });

        return success;
    }

    bool PluginInstanceVST3::UISupport::canResize() {
        if (!created || !view)
            return false;

        bool can_resize = false;
        EventLoop::runTaskOnMainThread([&] {
            can_resize = view->vtable->view.can_resize(view) == V3_TRUE;
        });

        return can_resize;
    }

    bool PluginInstanceVST3::UISupport::getSize(uint32_t &width, uint32_t &height) {
        if (!created || !view)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            v3_view_rect rect{};
            if (view->vtable->view.get_size(view, &rect) == V3_OK) {
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
            v3_view_rect rect{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
            success = view->vtable->view.on_size(view, &rect) == V3_OK;
        });

        return success;
    }

    bool PluginInstanceVST3::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        if (!created || !view)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            v3_view_rect rect{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
            if (view->vtable->view.check_size_constraint(view, &rect) == V3_OK) {
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
            success = scale_support->vtable->scale.set_content_scale_factor(scale_support, scale) == V3_OK;
        });

        return success;
    }

    void PluginInstanceVST3::UISupport::setResizeRequestHandler(std::function<bool(uint32_t, uint32_t)> handler) {
        host_resize_handler = std::move(handler);
        // Also set it on the HostApplication so it can delegate resize_view() calls
        // Use the view pointer as the key to identify this specific plugin instance
        if (view) {
            owner->owner->getHost()->setResizeRequestHandler(view, host_resize_handler);
        }
    }
}
