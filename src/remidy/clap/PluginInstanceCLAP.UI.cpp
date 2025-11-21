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

        // Check if the plugin supports the GUI extension
        const clap_plugin_gui_t* gui = (const clap_plugin_gui_t*)
            owner->plugin->get_extension(owner->plugin, CLAP_EXT_GUI);

        return gui != nullptr;
    }

    bool PluginInstanceCLAP::UISupport::ensureGuiExtension() {
        if (gui_ext)
            return true;
        if (!owner || !owner->plugin)
            return false;

        EventLoop::runTaskOnMainThread([&] {
            gui_ext = (const clap_plugin_gui_t*) owner->plugin->get_extension(owner->plugin, CLAP_EXT_GUI);
        });
        return gui_ext != nullptr;
    }

    bool PluginInstanceCLAP::UISupport::withGui(std::function<void()>&& func) {
        if (!created || !ensureGuiExtension())
            return false;

        EventLoop::runTaskOnMainThread(std::move(func));
        return true;
    }

    bool PluginInstanceCLAP::UISupport::tryCreateWith(const char* api, bool floating) {
        if (created || !ensureGuiExtension())
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            auto plugin = owner->plugin;
            if (gui_ext->is_api_supported && !gui_ext->is_api_supported(plugin, api, floating))
                return;
            if (!gui_ext->create || !gui_ext->create(plugin, api, floating))
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
        if (!ensureGuiExtension())
            return false;

        if (created)
            return false; // Already created

        host_resize_handler = resizeHandler;
        bool requestFloating = isFloating;

        auto tryFloating = [&]() -> bool {
            if (gui_ext->get_preferred_api) {
                const char* preferredApi{nullptr};
                bool preferredFloating{false};
                bool hasPreference = false;
                EventLoop::runTaskOnMainThread([&] {
                    hasPreference = gui_ext->get_preferred_api(owner->plugin, &preferredApi, &preferredFloating);
                });
                if (hasPreference && preferredFloating) {
                    if (tryCreateWith(preferredApi, true))
                        return true;
                }
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
            if (gui_ext->get_preferred_api) {
                EventLoop::runTaskOnMainThread([&] {
                    hasPreference = gui_ext->get_preferred_api(owner->plugin, &preferredApi, &preferredFloating);
                });
            }
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
                    if (!gui_ext->set_parent)
                        return;
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
                    attached_success = gui_ext->set_parent(owner->plugin, &window);
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
        if (!created || !ensureGuiExtension())
            return;

        withGui([&] {
            if (gui_ext->destroy)
                gui_ext->destroy(owner->plugin);
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
            if (gui_ext->show)
                success = gui_ext->show(owner->plugin);
        });
        if (success)
            visible = true;
        return success;
    }

    void PluginInstanceCLAP::UISupport::hide() {
        if (!created)
            return;
        withGui([&] {
            if (gui_ext->hide)
                gui_ext->hide(owner->plugin);
        });
        visible = false;
    }

    void PluginInstanceCLAP::UISupport::setWindowTitle(std::string title) {
        if (!created || !is_floating)
            return;
        withGui([&] {
            if (gui_ext->suggest_title)
                gui_ext->suggest_title(owner->plugin, title.c_str());
        });
    }

    bool PluginInstanceCLAP::UISupport::canResize() {
        if (!created || !ensureGuiExtension())
            return false;
        bool result = false;
        withGui([&] {
            if (gui_ext->can_resize)
                result = gui_ext->can_resize(owner->plugin);
        });
        return result;
    }

    bool PluginInstanceCLAP::UISupport::getSize(uint32_t &width, uint32_t &height) {
        if (!created || !ensureGuiExtension())
            return false;
        bool success = false;
        withGui([&] {
            if (gui_ext->get_size)
                success = gui_ext->get_size(owner->plugin, &width, &height);
        });
        return success;
    }

    bool PluginInstanceCLAP::UISupport::setSize(uint32_t width, uint32_t height) {
        if (!created || !ensureGuiExtension())
            return false;
        bool success = false;
        withGui([&] {
            if (gui_ext->set_size)
                success = gui_ext->set_size(owner->plugin, width, height);
        });
        return success;
    }

    bool PluginInstanceCLAP::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        if (!created || !ensureGuiExtension())
            return false;
        bool success = false;
        withGui([&] {
            if (gui_ext->adjust_size)
                success = gui_ext->adjust_size(owner->plugin, &width, &height);
        });
        return success;
    }

    bool PluginInstanceCLAP::UISupport::setScale(double scale) {
        if (!created || !ensureGuiExtension())
            return false;
        bool success = false;
        withGui([&] {
            if (gui_ext->set_scale)
                success = gui_ext->set_scale(owner->plugin, scale);
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
