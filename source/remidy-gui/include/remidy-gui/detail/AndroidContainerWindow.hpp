#pragma once

#if defined(__ANDROID__)

#include <jni.h>
#include <functional>

namespace remidy::gui::android {

    // Attaches the plugin-provided SurfaceView to the ContainerWindow overlay referenced by handle.
    void attachSurfaceView(void* windowHandle, jobject surfaceView);

    // Removes any currently attached SurfaceView from the overlay.
    void detachSurfaceView(void* windowHandle);

    // Retrieves the current Android host viewport size.
    void queryDimensions(void* windowHandle, int& width, int& height);

    // Converts Android pixel sizes to the units expected by ContainerWindow::resize().
    void androidPixelsToWindowSize(int& width, int& height);

    // Resizes the Android overlay content directly in Android pixels.
    void resizeContentPixels(void* windowHandle, int width, int height);

    // Queries the preferred remote UI content size via the Java SurfaceControl client path.
    bool queryRemoteViewPreferredSize(
        const char* pluginPackageName,
        const char* pluginId,
        int instanceId,
        int& width,
        int& height);

    // Registers a callback that fires once the attached SurfaceView is ready to connect.
    void setSurfaceReadyCallback(void* windowHandle, std::function<void()> callback);

    // Registers a callback for host viewport changes and scroll position updates.
    void setViewportCallback(
        void* windowHandle,
        std::function<void(int, int, int, int, int, int)> callback);

    // Called when the Kotlin overlay close button is pressed.
    void notifyOverlayClosed(void* windowHandle);

}

#endif
