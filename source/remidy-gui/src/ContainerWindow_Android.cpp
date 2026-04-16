#if defined(__ANDROID__)

#include <utility>
#include <string>
#include <jni.h>
#include "remidy-gui/remidy-gui.hpp"

namespace {

extern "C" void* SDL_GetAndroidJNIEnv();

JNIEnv* getEnv() {
    return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

jclass getOverlayManagerClass(JNIEnv* env) {
    static jclass cached = nullptr;
    if (!env)
        return nullptr;
    if (!cached) {
        auto local = env->FindClass("dev/atsushieno/uapmd/ui/PluginUiOverlayManager");
        if (!local)
            return nullptr;
        cached = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    return cached;
}

void callCreateOverlay(JNIEnv* env, jlong handle, jstring title, jint width, jint height) {
    auto cls = getOverlayManagerClass(env);
    if (!cls)
        return;
    auto mid = env->GetStaticMethodID(cls, "createOverlay", "(JLjava/lang/String;II)V");
    if (!mid)
        return;
    env->CallStaticVoidMethod(cls, mid, handle, title, width, height);
}

void callDestroyOverlay(JNIEnv* env, jlong handle) {
    auto cls = getOverlayManagerClass(env);
    if (!cls)
        return;
    auto mid = env->GetStaticMethodID(cls, "destroyOverlay", "(J)V");
    if (!mid)
        return;
    env->CallStaticVoidMethod(cls, mid, handle);
}

void callSetVisible(JNIEnv* env, jlong handle, bool visible) {
    auto cls = getOverlayManagerClass(env);
    if (!cls)
        return;
    auto mid = env->GetStaticMethodID(cls, "setOverlayVisible", "(JZ)V");
    if (!mid)
        return;
    env->CallStaticVoidMethod(cls, mid, handle, static_cast<jboolean>(visible));
}

void callResize(JNIEnv* env, jlong handle, jint width, jint height) {
    auto cls = getOverlayManagerClass(env);
    if (!cls)
        return;
    auto mid = env->GetStaticMethodID(cls, "resizeOverlay", "(JII)V");
    if (!mid)
        return;
    env->CallStaticVoidMethod(cls, mid, handle, width, height);
}

void constrainContentSize(JNIEnv* env, jint& width, jint& height) {
    auto cls = getOverlayManagerClass(env);
    if (!cls)
        return;
    auto mid = env->GetStaticMethodID(cls, "constrainContentSize", "(II)[I");
    if (!mid)
        return;
    auto result = static_cast<jintArray>(env->CallStaticObjectMethod(cls, mid, width, height));
    if (!result)
        return;
    if (env->GetArrayLength(result) >= 2) {
        jint values[2] = {};
        env->GetIntArrayRegion(result, 0, 2, values);
        width = values[0];
        height = values[1];
    }
    env->DeleteLocalRef(result);
}

void callAttachSurfaceView(JNIEnv* env, jlong handle, jobject surfaceView) {
    auto cls = getOverlayManagerClass(env);
    if (!cls)
        return;
    auto mid = env->GetStaticMethodID(cls, "attachSurfaceView", "(JLandroid/view/View;)V");
    if (!mid)
        return;
    env->CallStaticVoidMethod(cls, mid, handle, surfaceView);
}

}

namespace remidy::gui {

namespace {
constexpr int kFallbackDimension = 200;
constexpr int kAndroidUiScale = 2;

int scaledDimension(int value) {
    return (value > 0 ? value : kFallbackDimension) * kAndroidUiScale;
}

int unscaledDimension(int value) {
    return (value > 0 ? value : kFallbackDimension) / kAndroidUiScale;
}
} // namespace

class AndroidContainerWindow : public ContainerWindow {
public:
    AndroidContainerWindow(const char* title, int width, int height, std::function<void()> closeCallback)
        : title_(title ? title : "Plugin UI"),
          closeCallback_(std::move(closeCallback)),
          overlayHandle_(reinterpret_cast<jlong>(this)) {
        width_ = scaledDimension(width);
        height_ = scaledDimension(height);
        if (auto* env = getEnv()) {
            constrainContentSize(env, width_, height_);
            auto jTitle = env->NewStringUTF(title_.c_str());
            callCreateOverlay(env, overlayHandle_, jTitle, width_, height_);
            env->DeleteLocalRef(jTitle);
        }
    }

    ~AndroidContainerWindow() override {
        if (auto* env = getEnv()) {
            detachSurface();
            callDestroyOverlay(env, overlayHandle_);
        }
    }

    void show(bool visible) override {
        if (visible_ == visible)
            return;
        visible_ = visible;
        if (auto* env = getEnv())
            callSetVisible(env, overlayHandle_, visible);
    }

    void resize(int width, int height) override {
        width_ = scaledDimension(width);
        height_ = scaledDimension(height);
        if (auto* env = getEnv()) {
            constrainContentSize(env, width_, height_);
            callResize(env, overlayHandle_, width_, height_);
        }
        if (resizeCallback_)
            resizeCallback_(width_, height_);
    }

    void resizeContentPixels(int width, int height) {
        width_ = width > 0 ? width : kFallbackDimension;
        height_ = height > 0 ? height : kFallbackDimension;
        if (auto* env = getEnv()) {
            constrainContentSize(env, width_, height_);
            callResize(env, overlayHandle_, width_, height_);
        }
    }

    void setResizeCallback(std::function<void(int, int)> callback) override {
        resizeCallback_ = std::move(callback);
    }

    void setResizable(bool) override {
        // Surface-based overlays do not currently support host-driven resize handles.
    }

    Bounds getBounds() const override {
        return Bounds{0, 0, width_, height_};
    }

    void* getHandle() const override {
        return reinterpret_cast<void*>(const_cast<AndroidContainerWindow*>(this));
    }

    void attachSurface(jobject surfaceView) {
        attachedView_ = surfaceView;
        if (auto* env = getEnv())
            callAttachSurfaceView(env, overlayHandle_, surfaceView);
    }

    void detachSurface() {
        if (auto* env = getEnv())
            callAttachSurfaceView(env, overlayHandle_, nullptr);
        attachedView_ = nullptr;
    }

    void requestCloseFromOverlay() {
        if (closeCallback_)
            closeCallback_();
    }

private:
    std::string title_;
    int width_{0};
    int height_{0};
    std::function<void(int, int)> resizeCallback_{};
    std::function<void()> closeCallback_{};
    bool visible_{false};
    jlong overlayHandle_{0};
    jobject attachedView_{nullptr};
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height, std::function<void()> closeCallback) {
    return std::make_unique<AndroidContainerWindow>(title, width, height, std::move(closeCallback));
}

} // namespace remidy::gui

namespace remidy::gui::android {

void attachSurfaceView(void* windowHandle, jobject surfaceView) {
    if (!windowHandle)
        return;
    auto* window = reinterpret_cast<AndroidContainerWindow*>(windowHandle);
    window->attachSurface(surfaceView);
}

void detachSurfaceView(void* windowHandle) {
    if (!windowHandle)
        return;
    auto* window = reinterpret_cast<AndroidContainerWindow*>(windowHandle);
    window->detachSurface();
}

void queryDimensions(void* windowHandle, int& width, int& height) {
    if (!windowHandle)
        return;
    auto* window = reinterpret_cast<AndroidContainerWindow*>(windowHandle);
    auto bounds = window->getBounds();
    width = bounds.width;
    height = bounds.height;
}

void androidPixelsToWindowSize(int& width, int& height) {
    width = unscaledDimension(width);
    height = unscaledDimension(height);
}

void resizeContentPixels(void* windowHandle, int width, int height) {
    if (!windowHandle)
        return;
    auto* window = reinterpret_cast<AndroidContainerWindow*>(windowHandle);
    window->resizeContentPixels(width, height);
}

void notifyOverlayClosed(void* windowHandle) {
    if (!windowHandle)
        return;
    auto* window = reinterpret_cast<AndroidContainerWindow*>(windowHandle);
    window->requestCloseFromOverlay();
}

} // namespace remidy::gui::android

extern "C" JNIEXPORT void JNICALL
Java_dev_atsushieno_uapmd_MainActivity_nativeOnOverlayClosed(
    JNIEnv*, jclass, jlong handle)
{
    remidy::gui::android::notifyOverlayClosed(reinterpret_cast<void*>(handle));
}

#endif // __ANDROID__
