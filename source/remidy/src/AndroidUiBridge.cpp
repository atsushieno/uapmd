#include "AndroidUiBridge.hpp"
#include <jni.h>
#include <android/log.h>
#include <map>
#include <mutex>
#include <atomic>

#define LOG_TAG "remidy-ui"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

namespace remidy {

namespace {

std::mutex g_ui_task_mutex;
std::map<jlong, std::function<void()>> g_ui_tasks;
std::atomic<jlong> g_next_ui_task_id{1};

jobject g_activity = nullptr;
JavaVM* g_vm = nullptr;

bool isAndroidUiThread(JNIEnv* env)
{
    if (!env || !g_activity)
        return false;

    jclass activityClass = env->GetObjectClass(g_activity);
    if (!activityClass)
        return false;
    jmethodID getMainLooper = env->GetMethodID(activityClass, "getMainLooper", "()Landroid/os/Looper;");
    env->DeleteLocalRef(activityClass);
    if (!getMainLooper)
        return false;

    jobject mainLooper = env->CallObjectMethod(g_activity, getMainLooper);
    if (!mainLooper)
        return false;

    jclass looperClass = env->FindClass("android/os/Looper");
    if (!looperClass) {
        env->DeleteLocalRef(mainLooper);
        return false;
    }
    jmethodID myLooper = env->GetStaticMethodID(looperClass, "myLooper", "()Landroid/os/Looper;");
    if (!myLooper) {
        env->DeleteLocalRef(looperClass);
        env->DeleteLocalRef(mainLooper);
        return false;
    }

    jobject currentLooper = env->CallStaticObjectMethod(looperClass, myLooper);
    bool isUiThread = currentLooper && env->IsSameObject(mainLooper, currentLooper);
    if (currentLooper)
        env->DeleteLocalRef(currentLooper);
    env->DeleteLocalRef(looperClass);
    env->DeleteLocalRef(mainLooper);
    return isUiThread;
}

jclass mainActivityClass(JNIEnv* env)
{
    static jclass cached = nullptr;
    if (cached)
        return cached;
    jclass local = env->FindClass("dev/atsushieno/uapmd/MainActivity");
    if (!local)
        return nullptr;
    cached = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return cached;
}

jmethodID postNativeUiTaskMethod(JNIEnv* env)
{
    static jmethodID method = nullptr;
    if (method)
        return method;
    jclass cls = mainActivityClass(env);
    if (!cls)
        return nullptr;
    method = env->GetStaticMethodID(cls, "postNativeUiTask", "(J)V");
    return method;
}

jlong enqueueUiTask(std::function<void()> task)
{
    const jlong token = g_next_ui_task_id.fetch_add(1);
    std::lock_guard lock(g_ui_task_mutex);
    g_ui_tasks[token] = std::move(task);
    return token;
}

} // anonymous namespace

void initAndroidUiBridge(JNIEnv* env, jobject activity)
{
    if (g_activity) {
        env->DeleteGlobalRef(g_activity);
    }
    g_activity = env->NewGlobalRef(activity);
    env->GetJavaVM(&g_vm);
}

void runOnAndroidUiThread(std::function<void()> task)
{
    if (!g_vm) {
        LOGW("Android UI bridge not initialized; executing task immediately.");
        task();
        return;
    }

    JNIEnv* env = nullptr;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
             LOGW("Failed to attach thread to JVM; executing task immediately.");
             task();
             return;
        }
    }

    jclass cls = mainActivityClass(env);
    jmethodID method = postNativeUiTaskMethod(env);
    if (!cls || !method) {
        LOGW("MainActivity UI bridge unavailable; executing task immediately.");
        task();
        return;
    }
    jlong token = enqueueUiTask(std::move(task));
    env->CallStaticVoidMethod(cls, method, token);
}

void runOnAndroidUiThreadSync(std::function<void()> task)
{
    if (g_vm) {
        JNIEnv* env = nullptr;
        if (g_vm->GetEnv((void**) &env, JNI_VERSION_1_6) == JNI_OK && isAndroidUiThread(env)) {
            task();
            return;
        }
    }
    std::atomic<bool> done{false};
    runOnAndroidUiThread([&] {
        task();
        done = true;
        done.notify_one();
    });
    done.wait(false);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_atsushieno_uapmd_MainActivity_nativeExecuteUiThreadTask(
    JNIEnv*, jclass, jlong token)
{
    std::function<void()> fn;
    {
        std::lock_guard lock(g_ui_task_mutex);
        auto it = g_ui_tasks.find(token);
        if (it != g_ui_tasks.end()) {
            fn = std::move(it->second);
            g_ui_tasks.erase(it);
        }
    }
    if (fn)
        fn();
}

} // namespace remidy
