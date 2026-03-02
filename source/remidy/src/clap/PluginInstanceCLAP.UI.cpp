#include "PluginFormatCLAP.hpp"

#include <clap/ext/gui.h>
#include <cstdint>
#include <functional>
#include <utility>

namespace remidy {

    PluginInstanceCLAP::UISupport::UISupport(PluginInstanceCLAP* owner)
        : owner(owner) {}

    bool PluginInstanceCLAP::UISupport::hasUI() {
        if (!owner || !owner->plugin)
            return false;

        return owner->plugin->canUseGui();
    }

    bool PluginInstanceCLAP::UISupport::withGui(std::function<void()>&& func) {
        if (!created || !owner || !owner->plugin || !owner->plugin->canUseGui())
            return false;

        EventLoop::runTaskOnMainThread(std::move(func));
        return true;
    }

    bool PluginInstanceCLAP::UISupport::tryCreateWith(const char* api, bool floating) {
        if (created || !owner || !owner->plugin || !owner->plugin->canUseGui())
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            if (api && !owner->plugin->guiIsApiSupported(api, floating))
                return;
            if (!owner->plugin->guiCreate(api, floating))
                return;
            success = true;
        });

        if (success) {
            created = true;
            visible = false;
            is_floating = floating;
            current_api = api ? api : "";
        }

        return success;
    }

    bool PluginInstanceCLAP::UISupport::create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
        if (!owner || !owner->plugin || !owner->plugin->canUseGui())
            return false;

        if (created)
            return false; // Already created

        host_resize_handler = resizeHandler;
        bool requestFloating = isFloating;

        auto tryFloating = [&]() -> bool {
            const char* preferredApi{nullptr};
            bool preferredFloating{false};
            bool hasPreference = false;
            EventLoop::runTaskOnMainThread([&] {
                hasPreference = owner->plugin->guiGetPreferredApi(&preferredApi, &preferredFloating);
            });
            if (hasPreference && preferredFloating) {
                if (tryCreateWith(preferredApi, true))
                    return true;
            }
            if (tryCreateWith(nullptr, true))
                return true;
            static constexpr const char* floatingApis[] = {
#if defined(__APPLE__)
                CLAP_WINDOW_API_COCOA,
#endif
#if defined(_WIN32)
                CLAP_WINDOW_API_WIN32,
#endif
                CLAP_WINDOW_API_X11
            };
            for (auto api : floatingApis) {
                if (tryCreateWith(api, true))
                    return true;
            }
            return false;
        };

        auto tryEmbedded = [&]() -> bool {
            const char* preferredApi{nullptr};
            bool preferredFloating{false};
            bool hasPreference = false;
            EventLoop::runTaskOnMainThread([&] {
                hasPreference = owner->plugin->guiGetPreferredApi(&preferredApi, &preferredFloating);
            });
            if (hasPreference && !preferredFloating && preferredApi) {
                if (tryCreateWith(preferredApi, false))
                    return true;
            }
            static constexpr const char* embeddedApis[] = {
#if defined(__APPLE__)
                CLAP_WINDOW_API_COCOA,
#endif
#if defined(_WIN32)
                CLAP_WINDOW_API_WIN32,
#endif
                CLAP_WINDOW_API_X11
            };
            for (auto api : embeddedApis) {
                if (tryCreateWith(api, false))
                    return true;
            }
            return false;
        };

        bool success = false;
        if (requestFloating) {
            success = tryFloating();
        } else {
            success = tryEmbedded();
            // Attach to parent if embedded and creation succeeded
            if (success && parentHandle && !current_api.empty()) {
                bool attached_success = false;
                withGui([&] {
                    clap_window_t window{};
                    window.api = current_api.c_str();
                    if (current_api == CLAP_WINDOW_API_COCOA) {
                        window.cocoa = parentHandle;
                    }
#if defined(_WIN32)
                    else if (current_api == CLAP_WINDOW_API_WIN32) {
                        window.win32 = parentHandle;
                    }
#endif
                    else if (current_api == CLAP_WINDOW_API_X11) {
                        window.x11 = static_cast<clap_xwnd>(reinterpret_cast<uintptr_t>(parentHandle));
                    } else {
                        window.ptr = parentHandle;
                    }
                    attached_success = owner->plugin->guiSetParent(&window);
                });

                if (attached_success) {
                    attached = true;

                    // Notify host of initial UI size
                    if (host_resize_handler) {
                        uint32_t width = 0, height = 0;
                        if (getSize(width, height) && width > 0 && height > 0) {
                            host_resize_handler(width, height);
                        }
                    }
                } else {
                    // Failed to attach - destroy and fail
                    destroy();
                    return false;
                }
            }
        }

        return success;
    }

    void PluginInstanceCLAP::UISupport::destroy() {
        if (!created || !owner || !owner->plugin)
            return;

        withGui([&] {
            owner->plugin->guiDestroy();
        });

        created = false;
        visible = false;
        attached = false;
        current_api.clear();
        is_floating = true;
    }

    bool PluginInstanceCLAP::UISupport::show() {
        if (!created)
            return false;
        bool success = false;
        withGui([&] {
            success = owner->plugin->guiShow();
        });
        if (success)
            visible = true;
        return success;
    }

    void PluginInstanceCLAP::UISupport::hide() {
        if (!created)
            return;
        withGui([&] {
            owner->plugin->guiHide();
        });
        visible = false;
    }

    void PluginInstanceCLAP::UISupport::setWindowTitle(std::string title) {
        if (!created || !is_floating)
            return;
        withGui([&] {
            owner->plugin->guiSuggestTitle(title.c_str());
        });
    }

    bool PluginInstanceCLAP::UISupport::canResize() {
        if (!created || !owner || !owner->plugin)
            return false;
        bool result = false;
        withGui([&] {
            result = owner->plugin->guiCanResize();
        });
        return result;
    }

    bool PluginInstanceCLAP::UISupport::getSize(uint32_t &width, uint32_t &height) {
        if (!created || !owner || !owner->plugin)
            return false;
        bool success = false;
        withGui([&] {
            success = owner->plugin->guiGetSize(&width, &height);
        });
        return success;
    }

    bool PluginInstanceCLAP::UISupport::setSize(uint32_t width, uint32_t height) {
        if (!created || !owner || !owner->plugin)
            return false;
        bool success = false;
        withGui([&] {
            success = owner->plugin->guiSetSize(width, height);
        });
        return success;
    }

    bool PluginInstanceCLAP::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        if (!created || !owner || !owner->plugin)
            return false;
        bool success = false;
        withGui([&] {
            success = owner->plugin->guiAdjustSize(&width, &height);
        });
        return success;
    }

    bool PluginInstanceCLAP::UISupport::setScale(double scale) {
        if (!created || !owner || !owner->plugin)
            return false;
        bool success = false;
        withGui([&] {
            success = owner->plugin->guiSetScale(scale);
        });
        return success;
    }

    bool PluginInstanceCLAP::UISupport::handleGuiResize(uint32_t width, uint32_t height) {
        if (!created)
            return false;

        bool hostResized = true;
        if (host_resize_handler) {
            EventLoop::runTaskOnMainThread([&] {
                hostResized = host_resize_handler(width, height);
            });
        }

        if (!hostResized)
            return false;

        return setSize(width, height);
    }
}
