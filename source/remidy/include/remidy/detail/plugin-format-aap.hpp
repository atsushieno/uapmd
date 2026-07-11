#pragma once

#include "remidy/detail/plugin-format.hpp"

#if defined(__ANDROID__)
#include <functional>
#include <jni.h>
#endif

namespace remidy::gui::android {

#if defined(__ANDROID__)

    // Attaches the plugin-provided SurfaceView to the ContainerWindow overlay referenced by handle.
    inline void (*attachSurfaceView)(void* windowHandle, jobject surfaceView) = nullptr;

    // Removes any currently attached SurfaceView from the overlay.
    inline void (*detachSurfaceView)(void* windowHandle) = nullptr;

    // Retrieves the current Android host viewport size.
    inline void (*queryDimensions)(void* windowHandle, int& width, int& height) = nullptr;

    // Converts Android pixel sizes to the units expected by ContainerWindow::resize().
    void androidPixelsToWindowSize(int& width, int& height);

    // Resizes the Android overlay content directly in Android pixels.
    inline void (*resizeContentPixels)(void* windowHandle, int width, int height) = nullptr;

    // Queries the preferred remote UI content size via the Java SurfaceControl client path.
    bool queryRemoteViewPreferredSize(
        const char* pluginPackageName,
        const char* pluginId,
        int instanceId,
        int& width,
        int& height);

    // Registers a callback that fires once the attached SurfaceView is ready to connect.
    inline void (*setSurfaceReadyCallback)(void* windowHandle, std::function<void()> callback) = nullptr;

    // Registers a callback for host viewport changes and scroll position updates.
    inline void (*setViewportCallback)(
        void* windowHandle,
        std::function<void(int, int, int, int, int, int)> callback) = nullptr;

    // Called when the Kotlin overlay close button is pressed.
    inline void (*notifyOverlayClosed)(void* windowHandle) = nullptr;

#endif

}

namespace remidy {

    class PluginFormatAAP : public PluginFormat {
    public:
        PluginFormatAAP() = default;

        std::string name() override { return "AAP"; }

        PluginUIThreadRequirement requiresUIThreadOn(PluginCatalogEntry*) override { return PluginUIThreadRequirement::None; }

        bool canOmitUiState() override { return true; }

        bool isStateStructured() override { return false; }

        static std::unique_ptr<PluginFormatAAP> create();
    };
}
