// Android SDL3 entry point for UAPMD
// SDL3 provides the Activity infrastructure and main entry point
// Shared application logic is in main_common.cpp

#include "main_common.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <uapmd-file/IDocumentProvider.hpp>
#include <aap/core/android/android-application-context.h>
#include "../../remidy/src/AndroidUiBridge.hpp"
#endif

// SDL3 automatically provides the entry point for Android
// We just need to define main() which SDL3 will call
int main(int argc, char** argv) {
#if defined(__ANDROID__)
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env && activity) {
        aap::set_application_context(env, activity);
        remidy::initAndroidUiBridge(env, activity);
        uapmd::initDocumentProvider(env, activity);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "DocumentProvider init skipped: env=%p activity=%p", env, activity);
    }
#endif
    // Run the shared main application loop
    // SDL3 handles all Android lifecycle, windowing, and input
    return uapmd::runMainLoop(argc, argv);
}
