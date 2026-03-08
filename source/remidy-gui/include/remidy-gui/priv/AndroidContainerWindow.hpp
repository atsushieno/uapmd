#pragma once

#if defined(__ANDROID__)

#include <jni.h>

namespace remidy::gui::android {

    // Attaches the plugin-provided SurfaceView to the ContainerWindow overlay referenced by handle.
    void attachSurfaceView(void* windowHandle, jobject surfaceView);

    // Removes any currently attached SurfaceView from the overlay.
    void detachSurfaceView(void* windowHandle);

    // Retrieves the current size of the Android container window.
    void queryDimensions(void* windowHandle, int& width, int& height);

    // Called when the Kotlin overlay close button is pressed.
    void notifyOverlayClosed(void* windowHandle);

}

#endif
