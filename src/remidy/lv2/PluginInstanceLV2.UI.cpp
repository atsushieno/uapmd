#include "PluginFormatLV2.hpp"
#include <priv/event-loop.hpp>
#include <lv2/ui/ui.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/patch/patch.h>
#include <thread>
#include <chrono>

#ifdef __linux__
#include <X11/Xlib.h>
#include "../EventLoopLinux.hpp"
#include "remidy/lv2/wayland-ui.h"
#elif defined(__APPLE__)
#include <choc/platform/choc_ObjectiveCHelpers.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace remidy {

    PluginInstanceLV2::UISupport::UISupport(PluginInstanceLV2* owner) : owner(owner) {
    }

    bool PluginInstanceLV2::UISupport::hasUI() {
        // Check if the plugin has any UIs available for this platform
        LilvUIs* uis = lilv_plugin_get_uis(owner->plugin);
        if (!uis)
            return false;

        bool has_ui = lilv_uis_size(uis) > 0;
        lilv_uis_free(uis);
        return has_ui;
    }

    bool PluginInstanceLV2::UISupport::create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
        if (created)
            return false; // Already created - must call destroy() first

        is_floating = isFloating;
        parent_widget = parentHandle;
        host_resize_handler = resizeHandler;

        // Discover suitable UI for the platform
        if (!discoverUI())
            return false;

        // Instantiate the UI
        if (!instantiateUI())
            return false;

        // Attach to parent if not floating and parent provided
        if (!is_floating && parent_widget) {
            if (!ui_widget)
                return false;

            // Platform-specific embedding
#ifdef __linux__
#ifdef HAVE_WAYLAND
            // Check if we're using Wayland UI
            if (selected_ui_type && std::strcmp(selected_ui_type, LV2_UI__WaylandSurfaceUI) == 0) {
                // For WaylandSurfaceUI, the ui_widget returned by the plugin is a wl_subsurface*
                // The plugin handles subsurface creation using the parent wl_surface* passed via LV2_UI__parent
                // We don't need to do additional embedding - the plugin manages it
                // Just store the reference
                wayland_parent_surface = static_cast<wl_surface*>(parent_widget);
            } else
#endif
            {
                // X11 embedding (or XWayland)
                Display* dpy = XOpenDisplay(nullptr);
                if (!dpy)
                    return false;

                Window child = (Window)(uintptr_t)ui_widget;
                Window parent_window = (Window)(uintptr_t)parent_widget;
                XReparentWindow(dpy, child, parent_window, 0, 0);
                XMapWindow(dpy, child);
                XFlush(dpy);
                XCloseDisplay(dpy);
            }
#elif defined(__APPLE__)
            id child = (id)ui_widget;
            id parentView = (id)parent_widget;
            if (!child || !parentView)
                return false;

            // Get child's preferred size before modifying frame
            auto childFrame = choc::objc::call<choc::objc::CGRect>(child, "frame");
            uint32_t preferredWidth = (uint32_t)childFrame.size.width;
            uint32_t preferredHeight = (uint32_t)childFrame.size.height;

            // Add as subview
            choc::objc::call<void>(parentView, "addSubview:", child);

            // Set frame to fill parent (origin at 0,0)
            auto parentBounds = choc::objc::call<choc::objc::CGRect>(parentView, "bounds");
            choc::objc::call<void>(child, "setFrame:", parentBounds);

            // Enable autoresizing
            choc::objc::call<void>(child, "setAutoresizingMask:", (uint64_t)(1 << 1 | 1 << 4));

            // Notify host of plugin's preferred UI size
            if (host_resize_handler && preferredWidth > 0 && preferredHeight > 0) {
                host_resize_handler(preferredWidth, preferredHeight);
            }
#elif defined(_WIN32)
            HWND child = (HWND)ui_widget;
            HWND parentWnd = (HWND)parent_widget;
            if (!child || !parentWnd)
                return false;
            SetParent(child, parentWnd);
            ShowWindow(child, SW_SHOW);

            // Notify host of initial UI size
            uint32_t width = 0, height = 0;
            if (host_resize_handler && getSize(width, height) && width > 0 && height > 0) {
                host_resize_handler(width, height);
            }
#else
            return false;
#endif

#ifdef __linux__
            // Notify host of initial UI size (for X11 and Wayland)
            uint32_t width = 0, height = 0;
            if (host_resize_handler && getSize(width, height) && width > 0 && height > 0) {
                host_resize_handler(width, height);
            }
#endif
        }

        created = true;
        return true;
    }

    bool PluginInstanceLV2::UISupport::discoverUI() {
        available_uis = lilv_plugin_get_uis(owner->plugin);
        if (!available_uis || lilv_uis_size(available_uis) == 0)
            return false;

        // Platform-specific UI type preferences
        const char* preferred_types[10];
        int type_count = 0;

#ifdef __linux__
        // On Linux, auto-detect Wayland vs X11 and prefer accordingly
        auto* loop = dynamic_cast<EventLoopLinux*>(getEventLoop());
        if (loop && loop->getDisplayServerType() == DisplayServerType::Wayland) {
            // Prefer Wayland native UI
            preferred_types[type_count++] = LV2_UI__WaylandSurfaceUI;
            // Fallback to X11 via XWayland if available
            preferred_types[type_count++] = LV2_UI__X11UI;
        } else {
            // On X11, prefer X11 UI
            preferred_types[type_count++] = LV2_UI__X11UI;
        }
#elif defined(__APPLE__)
        preferred_types[type_count++] = LV2_UI__CocoaUI;
#elif defined(_WIN32)
        preferred_types[type_count++] = LV2_UI__WindowsUI;
#endif
        preferred_types[type_count] = nullptr;

        // Find first supported UI type
        for (int i = 0; preferred_types[i] != nullptr; ++i) {
            LilvNode* type_node = lilv_new_uri(owner->formatImpl->world, preferred_types[i]);
            LILV_FOREACH(uis, j, available_uis) {
                const LilvUI* ui = lilv_uis_get(available_uis, j);
                if (lilv_ui_is_a(ui, type_node)) {
                    selected_ui = ui;
                    selected_ui_type = preferred_types[i];
                    lilv_node_free(type_node);
                    return true;
                }
            }
            lilv_node_free(type_node);
        }

        return false;
    }

    std::vector<const LV2_Feature*> PluginInstanceLV2::UISupport::buildFeatures() {
        std::vector<const LV2_Feature*> features;

        // Add resize feature (host provides resize capability to UI)
        host_resize_feature.handle = this;
        host_resize_feature.ui_resize = resizeUI;
        static LV2_Feature resize_feature = { LV2_UI__resize, &host_resize_feature };
        features.push_back(&resize_feature);

        // Add port map feature (allows UI to map port symbols to indices)
        port_map_feature.handle = this;
        port_map_feature.port_index = portIndex;
        static LV2_Feature port_map_feature_struct = { LV2_UI__portMap, &port_map_feature };
        features.push_back(&port_map_feature_struct);

#ifdef HAVE_WAYLAND
        // Add Wayland display feature if using WaylandSurfaceUI
        if (selected_ui_type && std::strcmp(selected_ui_type, LV2_UI__WaylandSurfaceUI) == 0) {
            const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
            if (wayland_display && wayland_display[0] != '\0') {
                wl_display* display = wl_display_connect(wayland_display);

                // Provide the Wayland display connection via "urn:wayland:display" feature
                static LV2_Feature wayland_display_feature = { LV2_WAYLAND_DISPLAY_URI, display };
                features.push_back(&wayland_display_feature);
            }
        }
#endif

        features.push_back(nullptr); // Null-terminate
        return features;
    }

    bool PluginInstanceLV2::UISupport::instantiateUI() {
        if (!selected_ui)
            return false;

        // Get UI binary path
        const LilvNode* binary_uri = lilv_ui_get_binary_uri(selected_ui);
        const char* binary_path = lilv_file_uri_parse(lilv_node_as_string(binary_uri), nullptr);
        if (!binary_path)
            return false;

        // Load UI library
#ifdef _WIN32
        ui_lib_handle = LoadLibraryA(binary_path);
#else
        ui_lib_handle = dlopen(binary_path, RTLD_NOW);
#endif

        lilv_free((void*)binary_path);

        if (!ui_lib_handle)
            return false;

        // Get descriptor function
#ifdef _WIN32
        auto desc_func = (LV2UI_DescriptorFunction)GetProcAddress((HMODULE)ui_lib_handle, "lv2ui_descriptor");
#else
        auto desc_func = (LV2UI_DescriptorFunction)dlsym(ui_lib_handle, "lv2ui_descriptor");
#endif

        if (!desc_func)
            return false;

        // Get descriptor (try index 0)
        ui_descriptor = desc_func(0);
        if (!ui_descriptor)
            return false;

        // Get base features from world context
        auto base_features = owner->formatImpl->worldContext->features.features;

        // Count base features
        size_t base_feature_count = 0;
        if (base_features) {
            while (base_features[base_feature_count] != nullptr)
                base_feature_count++;
        }

        // Build instance-specific feature structs first
        std::vector<LV2_Feature> ui_feature_structs;

        // Add instance access feature (gives UI access to plugin instance)
        LV2_Feature instance_access_feature;
        instance_access_feature.URI = LV2_INSTANCE_ACCESS_URI;
        instance_access_feature.data = lilv_instance_get_handle(owner->instance);
        ui_feature_structs.push_back(instance_access_feature);

        // Add parent feature if parent widget is available (required by JUCE LV2 plugins)
        if (parent_widget) {
            LV2_Feature parent_feature;
            parent_feature.URI = LV2_UI__parent;
            parent_feature.data = parent_widget;
            ui_feature_structs.push_back(parent_feature);
        }

        // Add resize feature (allows UI to request size changes from host)
        host_resize_feature.handle = this;
        host_resize_feature.ui_resize = resizeUI;
        LV2_Feature resize_feature;
        resize_feature.URI = LV2_UI__resize;
        resize_feature.data = &host_resize_feature;
        ui_feature_structs.push_back(resize_feature);

        // Add port map feature (allows UI to map port symbols to indices)
        port_map_feature.handle = this;
        port_map_feature.port_index = portIndex;
        LV2_Feature port_map_feature_wrapper;
        port_map_feature_wrapper.URI = LV2_UI__portMap;
        port_map_feature_wrapper.data = &port_map_feature;
        ui_feature_structs.push_back(port_map_feature_wrapper);

        // Now build the pointer array (after all structs are in place to avoid reallocation issues)
        std::vector<const LV2_Feature*> ui_features;

        // Add pointers to our instance-specific features
        for (auto& feature : ui_feature_structs) {
            ui_features.push_back(&feature);
        }

        // Append base features
        for (size_t i = 0; i < base_feature_count; i++) {
            ui_features.push_back(base_features[i]);
        }

        // Null-terminate
        ui_features.push_back(nullptr);

        // Get plugin and bundle URIs
        const char* plugin_uri = lilv_node_as_string(lilv_plugin_get_uri(owner->plugin));
        const LilvNode* bundle_uri_node = lilv_ui_get_bundle_uri(selected_ui);
        const char* bundle_path_uri = lilv_node_as_string(bundle_uri_node);
        const char* bundle_path = lilv_file_uri_parse(bundle_path_uri, nullptr);

        // Instantiate UI
        ui_handle = ui_descriptor->instantiate(
            ui_descriptor,
            plugin_uri,
            bundle_path,
            writeFunction,
            owner, // controller handle
            &ui_widget,
            ui_features.data()
        );

        lilv_free((void*)bundle_path);

        if (!ui_handle)
            return false;

        // Query optional interfaces
        if (ui_descriptor->extension_data) {
            idle_interface = (const LV2UI_Idle_Interface*)
                ui_descriptor->extension_data(LV2_UI__idleInterface);
            show_interface = (const LV2UI_Show_Interface*)
                ui_descriptor->extension_data(LV2_UI__showInterface);
            ui_resize_interface = (const LV2UI_Resize*)
                ui_descriptor->extension_data(LV2_UI__resize);
        }

        // Start idle timer if UI requires it
        if (idle_interface)
            startIdleTimer();

        return true;
    }

    void PluginInstanceLV2::UISupport::destroy() {
        if (!created)
            return;

        stopIdleTimer();

        if (ui_handle && ui_descriptor && ui_descriptor->cleanup) {
            ui_descriptor->cleanup(ui_handle);
            ui_handle = nullptr;
        }

        if (ui_lib_handle) {
#ifdef _WIN32
            FreeLibrary((HMODULE)ui_lib_handle);
#else
            dlclose(ui_lib_handle);
#endif
            ui_lib_handle = nullptr;
        }

        if (available_uis) {
            lilv_uis_free(available_uis);
            available_uis = nullptr;
        }

        ui_descriptor = nullptr;
        ui_widget = nullptr;
        selected_ui = nullptr;
        idle_interface = nullptr;
        show_interface = nullptr;
        ui_resize_interface = nullptr;

        created = false;
        visible = false;
        parent_widget = nullptr;
    }

    bool PluginInstanceLV2::UISupport::show() {
        if (!created)
            return false;

        // Instantiate UI if not already done
        if (!ui_handle && !instantiateUI())
            return false;

        if (show_interface) {
            if (show_interface->show(ui_handle) != 0)
                return false;
        }

        visible = true;
        return true;
    }

    void PluginInstanceLV2::UISupport::hide() {
        if (!created || !visible)
            return;

        if (show_interface) {
            show_interface->hide(ui_handle);
        }

        visible = false;
    }

    void PluginInstanceLV2::UISupport::setWindowTitle(std::string title) {
        window_title = title;
        // LV2 UIs receive title via options at instantiation time
        // Changing it after instantiation is not standardized
    }

    bool PluginInstanceLV2::UISupport::canResize() {
        if (!created || !ui_handle)
            return false;

        // Check if UI has resize interface
        return ui_resize_interface != nullptr;
    }

    bool PluginInstanceLV2::UISupport::getSize(uint32_t &width, uint32_t &height) {
        if (!created || !ui_widget)
            return false;

#ifdef __linux__
        Display* dpy = XOpenDisplay(nullptr);
        if (!dpy)
            return false;

        Window window = (Window)(uintptr_t)ui_widget;
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, window, &attrs)) {
            width = attrs.width;
            height = attrs.height;
            XCloseDisplay(dpy);
            return true;
        }
        XCloseDisplay(dpy);
        return false;
#elif defined(__APPLE__)
        id view = (id)ui_widget;
        if (!view)
            return false;
        auto frame = choc::objc::call<choc::objc::CGRect>(view, "frame");
        width = (uint32_t)frame.size.width;
        height = (uint32_t)frame.size.height;
        return true;
#elif defined(_WIN32)
        HWND hwnd = (HWND)ui_widget;
        if (!hwnd)
            return false;
        RECT rect;
        if (GetClientRect(hwnd, &rect)) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
            return true;
        }
        return false;
#else
        return false;
#endif
    }

    bool PluginInstanceLV2::UISupport::setSize(uint32_t width, uint32_t height) {
        if (!created)
            return false;

        // If UI has resize interface, tell the plugin to resize
        if (ui_resize_interface) {
            if (ui_resize_interface->ui_resize(ui_resize_interface->handle, width, height) != 0)
                return false;
        }

        // Also update the native widget frame
        if (ui_widget) {
#ifdef __APPLE__
            id view = (id)ui_widget;
            if (view) {
                auto frame = choc::objc::CGRect{{0.0, 0.0}, {(double)width, (double)height}};
                choc::objc::call<void>(view, "setFrame:", frame);
                return true;
            }
#elif defined(_WIN32)
            HWND hwnd = (HWND)ui_widget;
            if (hwnd) {
                SetWindowPos(hwnd, NULL, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                return true;
            }
#elif defined(__linux__)
            Display* dpy = XOpenDisplay(nullptr);
            if (dpy) {
                Window window = (Window)(uintptr_t)ui_widget;
                XResizeWindow(dpy, window, width, height);
                XFlush(dpy);
                XCloseDisplay(dpy);
                return true;
            }
#endif
        }

        return ui_resize_interface != nullptr;
    }

    bool PluginInstanceLV2::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        if (!setSize(width, height))
            return false;

        // Query actual size after resize
        return getSize(width, height);
    }

    bool PluginInstanceLV2::UISupport::setScale(double scale) {
        // LV2 UI scaling would be handled via options
        // Not implementing for now
        (void)scale;
        return false;
    }

    // Static callbacks

    void PluginInstanceLV2::UISupport::writeFunction(LV2UI_Controller controller,
                                                      uint32_t port_index,
                                                      uint32_t buffer_size,
                                                      uint32_t port_protocol,
                                                      const void* buffer) {
        auto* instance = (PluginInstanceLV2*)controller;
        auto& uridMap = instance->implContext.statics->features.urid_map_feature_data;

        Logger::global()->logDiagnostic("LV2 UI writeFunction: port_index=%u, buffer_size=%u, port_protocol=%u",
                                         port_index, buffer_size, port_protocol);

        // Handle float protocol (control ports)
        if (port_protocol == 0 && buffer_size == sizeof(float)) {
            float value = *(const float*)buffer;
            Logger::global()->logDiagnostic("LV2 UI control port change: port %u = %f", port_index, value);
            // UI sent this change, don't echo back to UI
            auto params = dynamic_cast<PluginInstanceLV2::ParameterSupport*>(instance->_parameters);
            if (params)
                params->setParameterInternal(port_index, value, 0, false);
            return;
        }

        // Handle atom protocol (for patch properties / parameters and other atom events)
        if (port_protocol == uridMap.map(uridMap.handle, LV2_ATOM__eventTransfer)) {
            const LV2_Atom* atom = (const LV2_Atom*)buffer;

            // Check if it's an object/blank atom
            if (atom->type == uridMap.map(uridMap.handle, LV2_ATOM__Object) ||
                atom->type == uridMap.map(uridMap.handle, LV2_ATOM__Blank)) {

                const LV2_Atom_Object* obj = (const LV2_Atom_Object*)atom;

                // Check if it's a patch:Set message for a known parameter
                LV2_URID patchSet = uridMap.map(uridMap.handle, LV2_PATCH__Set);
                if (obj->body.otype == patchSet) {
                    // Extract property and value from patch:Set
                    LV2_URID patchProperty = uridMap.map(uridMap.handle, LV2_PATCH__property);
                    LV2_URID patchValue = uridMap.map(uridMap.handle, LV2_PATCH__value);

                    const LV2_Atom* property = nullptr;
                    const LV2_Atom* value = nullptr;

                    // Iterate through object properties to find property and value
                    LV2_ATOM_OBJECT_FOREACH(obj, prop) {
                        if (prop->key == patchProperty) {
                            property = &prop->value;
                        } else if (prop->key == patchValue) {
                            value = &prop->value;
                        }
                    }

                    // If we found both property and value
                    if (property && value && property->type == uridMap.map(uridMap.handle, LV2_ATOM__URID)) {
                        LV2_URID propertyUrid = ((const LV2_Atom_URID*)property)->body;

                        // Find parameter index for this property
                        auto params = dynamic_cast<PluginInstanceLV2::ParameterSupport*>(instance->_parameters);
                        if (params) {
                            auto index = params->indexForProperty(propertyUrid);
                            if (index.has_value()) {
                                // Extract the value based on type
                                double doubleValue = 0.0;
                                LV2_URID floatType = uridMap.map(uridMap.handle, LV2_ATOM__Float);
                                LV2_URID doubleType = uridMap.map(uridMap.handle, LV2_ATOM__Double);
                                LV2_URID intType = uridMap.map(uridMap.handle, LV2_ATOM__Int);
                                LV2_URID longType = uridMap.map(uridMap.handle, LV2_ATOM__Long);
                                LV2_URID boolType = uridMap.map(uridMap.handle, LV2_ATOM__Bool);

                                if (value->type == floatType) {
                                    doubleValue = ((const LV2_Atom_Float*)value)->body;
                                } else if (value->type == doubleType) {
                                    doubleValue = ((const LV2_Atom_Double*)value)->body;
                                } else if (value->type == intType) {
                                    doubleValue = ((const LV2_Atom_Int*)value)->body;
                                } else if (value->type == longType) {
                                    doubleValue = ((const LV2_Atom_Long*)value)->body;
                                } else if (value->type == boolType) {
                                    doubleValue = ((const LV2_Atom_Bool*)value)->body ? 1.0 : 0.0;
                                } else {
                                    // Not a numeric value - fall through to forward entire atom to plugin
                                    Logger::global()->logDiagnostic("LV2 UI patch:Set with non-numeric value type %u, forwarding to plugin",
                                                                     value->type);
                                    instance->enqueueAtomEvent(port_index, buffer_size, port_protocol, buffer);
                                    return;
                                }

                                // Update DSP plugin and notify host listeners
                                // Note: Pass false for notifyUI to prevent feedback loop - UI already knows the value
                                Logger::global()->logDiagnostic("LV2 UI patch:Set: property URID=%u, index=%u, value=%f",
                                                                 propertyUrid, index.value(), doubleValue);
                                params->setParameterInternal(index.value(), doubleValue, 0, false);
                                return;
                            } else {
                                // Not a known parameter property - forward to plugin
                                Logger::global()->logDiagnostic("LV2 UI patch:Set: property URID=%u not found in parameter list, forwarding to plugin",
                                                                 propertyUrid);
                            }
                        }
                    }
                }
            }

            // For all atom events that aren't handled above (including MIDI, file paths, etc.),
            // forward them to the plugin's atom input port
            Logger::global()->logDiagnostic("LV2 UI atom event: port_index=%u, buffer_size=%u, protocol=%u, forwarding to plugin",
                                             port_index, buffer_size, port_protocol);
            instance->enqueueAtomEvent(port_index, buffer_size, port_protocol, buffer);
        } else {
            Logger::global()->logDiagnostic("LV2 UI writeFunction: unhandled protocol %u", port_protocol);
        }
    }

    uint32_t PluginInstanceLV2::UISupport::portIndex(LV2UI_Feature_Handle handle, const char* symbol) {
        auto* ui_support = (UISupport*)handle;
        auto* owner = ui_support->owner;

        LilvNode* symbol_node = lilv_new_string(owner->formatImpl->world, symbol);
        const LilvPort* port = lilv_plugin_get_port_by_symbol(owner->plugin, symbol_node);
        lilv_node_free(symbol_node);

        if (!port)
            return LV2UI_INVALID_PORT_INDEX;

        return lilv_port_get_index(owner->plugin, port);
    }

    int PluginInstanceLV2::UISupport::resizeUI(LV2UI_Feature_Handle handle, int width, int height) {
        auto* ui_support = (UISupport*)handle;

        if (ui_support->host_resize_handler) {
            return ui_support->host_resize_handler((uint32_t)width, (uint32_t)height) ? 0 : 1;
        }

        return 1; // Not supported
    }

    // Idle timer implementation

    void PluginInstanceLV2::UISupport::startIdleTimer() {
        if (idle_timer_running || !idle_interface)
            return;

        idle_timer_running = true;

        // Single background thread for timing, posts one callback at a time
        std::thread([this]() {
            while (idle_timer_running) {
                // Wait for 33ms before next idle call
                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30Hz

                if (!idle_timer_running)
                    break;

                // Post ONE idle callback and wait for it to be processed
                auto callback_done = std::make_shared<std::atomic<bool>>(false);

                EventLoop::enqueueTaskOnMainThread([this, callback_done]() {
                    if (idle_interface && ui_handle && idle_timer_running) {
                        int result = idle_interface->idle(ui_handle);

                        // If idle returns non-zero, UI wants to close
                        if (result != 0) {
                            hide();
                            stopIdleTimer();
                        }
                    }
                    *callback_done = true;
                });

                // Wait for callback to complete before scheduling next one
                while (!*callback_done && idle_timer_running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }).detach();
    }

    void PluginInstanceLV2::UISupport::stopIdleTimer() {
        idle_timer_running = false;
    }

    void PluginInstanceLV2::UISupport::scheduleIdleCallback() {
        // No longer used - keeping for compatibility
    }

    void PluginInstanceLV2::UISupport::notifyParameterChange(LV2_URID propertyUrid, double value) {
        if (!ui_handle || !ui_descriptor || !ui_descriptor->port_event)
            return;

        auto& uridMap = owner->implContext.statics->features.urid_map_feature_data;

        // Create a patch:Set atom to send to the UI via port_event
        uint8_t atomBuf[256];
        LV2_Atom_Forge forge;
        lv2_atom_forge_init(&forge, &uridMap);
        lv2_atom_forge_set_buffer(&forge, atomBuf, sizeof(atomBuf));

        LV2_Atom_Forge_Frame frame;
        LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_object(
            &forge, &frame, 0,
            uridMap.map(uridMap.handle, LV2_PATCH__Set));

        lv2_atom_forge_key(&forge, uridMap.map(uridMap.handle, LV2_PATCH__property));
        lv2_atom_forge_urid(&forge, propertyUrid);

        lv2_atom_forge_key(&forge, uridMap.map(uridMap.handle, LV2_PATCH__value));
        lv2_atom_forge_float(&forge, static_cast<float>(value));

        lv2_atom_forge_pop(&forge, &frame);

        // Find the control port (atom output port from plugin perspective, input to UI)
        // This is typically the first atom port
        uint32_t controlPortIndex = 0;
        for (uint32_t i = 0; i < lilv_plugin_get_num_ports(owner->plugin); i++) {
            auto port = lilv_plugin_get_port_by_index(owner->plugin, i);
            if (owner->implContext.IS_ATOM_OUT(owner->plugin, port)) {
                controlPortIndex = i;
                break;
            }
        }

        // Call port_event to notify the UI
        ui_descriptor->port_event(
            ui_handle,
            controlPortIndex,
            lv2_atom_total_size(msg),
            uridMap.map(uridMap.handle, LV2_ATOM__eventTransfer),
            msg
        );

        Logger::global()->logDiagnostic("LV2: Notified UI via port_event - port=%u, property URID=%u, value=%f",
                                         controlPortIndex, propertyUrid, value);
    }

}
