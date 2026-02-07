// Android NativeActivity entry point for UAPMD
// For desktop, see main.cpp
// Shared application logic is in main_common.cpp

#include "main_common.hpp"
#include <android_native_app_glue.h>
#include <android/log.h>
#include <jni.h>
#include <imgui.h>

// Android logging macros
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "UAPMD", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "UAPMD", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "UAPMD", __VA_ARGS__)

// Global Android app state (referenced by main_common.cpp)
struct android_app* g_AndroidApp = nullptr;

/**
 * Handle Android app lifecycle commands.
 */
static void handleAppCmd(struct android_app* app, int32_t appCmd) {
    switch (appCmd) {
        case APP_CMD_INIT_WINDOW:
            LOGI("APP_CMD_INIT_WINDOW: Window created");
            // Window is ready - initialization happens in android_main when we detect this
            break;

        case APP_CMD_TERM_WINDOW:
            LOGI("APP_CMD_TERM_WINDOW: Window being destroyed");
            // Window is being destroyed - cleanup handled in main loop
            break;

        case APP_CMD_GAINED_FOCUS:
            LOGI("APP_CMD_GAINED_FOCUS: App gained focus");
            break;

        case APP_CMD_LOST_FOCUS:
            LOGI("APP_CMD_LOST_FOCUS: App lost focus");
            break;

        case APP_CMD_PAUSE:
            LOGI("APP_CMD_PAUSE: App paused");
            break;

        case APP_CMD_RESUME:
            LOGI("APP_CMD_RESUME: App resumed");
            break;

        case APP_CMD_SAVE_STATE:
            LOGI("APP_CMD_SAVE_STATE: Save app state");
            // TODO: Save application state if needed
            break;

        case APP_CMD_DESTROY:
            LOGI("APP_CMD_DESTROY: App being destroyed");
            break;
    }
}

/**
 * Handle Android input events.
 * Currently delegated to SDL3 - this is a hook point for custom input handling if needed.
 */
static int32_t handleInputEvent(struct android_app* app, AInputEvent* inputEvent) {
    // Return 0 to let SDL3 process the event through its own mechanisms
    // Return 1 if we handle it here and want to consume it
    return 0;
}

/**
 * JNI helper: Show soft keyboard.
 * Called from main_common.cpp when ImGui wants text input.
 */
int showSoftKeyboardInput() {
    if (!g_AndroidApp) {
        LOGE("showSoftKeyboardInput: g_AndroidApp is null");
        return -1;
    }

    JavaVM* javaVM = g_AndroidApp->activity->vm;
    JNIEnv* jniEnv = nullptr;

    jint result = javaVM->GetEnv((void**)&jniEnv, JNI_VERSION_1_6);
    if (result == JNI_ERR) {
        LOGE("showSoftKeyboardInput: GetEnv failed");
        return -1;
    }

    result = javaVM->AttachCurrentThread(&jniEnv, nullptr);
    if (result != JNI_OK) {
        LOGE("showSoftKeyboardInput: AttachCurrentThread failed");
        return -2;
    }

    jclass activityClass = jniEnv->GetObjectClass(g_AndroidApp->activity->clazz);
    if (!activityClass) {
        LOGE("showSoftKeyboardInput: GetObjectClass failed");
        javaVM->DetachCurrentThread();
        return -3;
    }

    jmethodID methodID = jniEnv->GetMethodID(activityClass, "showSoftInput", "()V");
    if (!methodID) {
        LOGE("showSoftKeyboardInput: GetMethodID for showSoftInput failed");
        javaVM->DetachCurrentThread();
        return -4;
    }

    jniEnv->CallVoidMethod(g_AndroidApp->activity->clazz, methodID);
    javaVM->DetachCurrentThread();

    LOGI("Soft keyboard shown");
    return 0;
}

/**
 * JNI helper: Poll Unicode characters from Java keyboard queue.
 * Called from main_common.cpp each frame to get text input.
 */
int pollUnicodeChars() {
    if (!g_AndroidApp) {
        return -1;
    }

    JavaVM* javaVM = g_AndroidApp->activity->vm;
    JNIEnv* jniEnv = nullptr;

    jint result = javaVM->GetEnv((void**)&jniEnv, JNI_VERSION_1_6);
    if (result == JNI_ERR) {
        return -1;
    }

    result = javaVM->AttachCurrentThread(&jniEnv, nullptr);
    if (result != JNI_OK) {
        return -2;
    }

    jclass activityClass = jniEnv->GetObjectClass(g_AndroidApp->activity->clazz);
    if (!activityClass) {
        javaVM->DetachCurrentThread();
        return -3;
    }

    jmethodID methodID = jniEnv->GetMethodID(activityClass, "pollUnicodeChar", "()I");
    if (!methodID) {
        javaVM->DetachCurrentThread();
        return -4;
    }

    // Poll all pending characters from the queue
    ImGuiIO& io = ImGui::GetIO();
    jint unicodeChar;
    while ((unicodeChar = jniEnv->CallIntMethod(g_AndroidApp->activity->clazz, methodID)) != 0) {
        io.AddInputCharacter(unicodeChar);
    }

    javaVM->DetachCurrentThread();
    return 0;
}

/**
 * Android NativeActivity entry point.
 * Called by the Android system when the app launches.
 */
void android_main(struct android_app* app) {
    LOGI("android_main() entered");

    // Store global app pointer for access by main_common.cpp and JNI helpers
    g_AndroidApp = app;

    // Register lifecycle and input event handlers
    app->onAppCmd = handleAppCmd;
    app->onInputEvent = handleInputEvent;

    LOGI("Waiting for window initialization...");

    // Wait for window to be created (block until APP_CMD_INIT_WINDOW)
    while (app->window == nullptr) {
        int events;
        struct android_poll_source* source;

        // Block with -1 timeout until we get an event
        if (ALooper_pollOnce(-1, nullptr, &events, (void**)&source) >= 0) {
            // Process this event
            if (source != nullptr) {
                source->process(app, source);
            }

            // Check for early exit
            if (app->destroyRequested != 0) {
                LOGI("Destroy requested before window creation, exiting");
                return;
            }
        }
    }

    LOGI("Window available, starting main application loop");

    // Create fake argc/argv for the main loop
    // Android apps don't have command-line arguments, so we just pass the app name
    const char* argv[] = { "uapmd-app" };
    int argc = 1;

    // Run the shared main application loop
    int result = uapmd::runMainLoop(argc, const_cast<char**>(argv));

    LOGI("Main loop exited with code %d", result);

    // Android will clean up automatically when this function returns
    // The NativeActivity framework handles the teardown
}
