#if defined(__ANDROID__)

#include "remidy/remidy.hpp"
#include "../AndroidUiBridge.hpp"
#include <jni.h>

namespace remidy::gui::android {

namespace {

constexpr int kFallbackDimension = 200;
constexpr int kAndroidUiScale = 1;

int unscaledDimension(int value) {
    return (value > 0 ? value : kFallbackDimension) / kAndroidUiScale;
}

bool callQueryRemoteViewPreferredSize(
    JNIEnv* env,
    const char* pluginPackageName,
    const char* pluginId,
    jint instanceId,
    jint& width,
    jint& height)
{
    if (!env)
        return false;
    auto cls = env->FindClass("dev/atsushieno/uapmd/MainActivity");
    if (!cls)
        return false;
    auto mid = env->GetStaticMethodID(
        cls,
        "queryRemoteViewPreferredSize",
        "(Ljava/lang/String;Ljava/lang/String;I)[I");
    if (!mid) {
        env->DeleteLocalRef(cls);
        return false;
    }
    auto jPackageName = env->NewStringUTF(pluginPackageName ? pluginPackageName : "");
    auto jPluginId = env->NewStringUTF(pluginId ? pluginId : "");
    auto result = static_cast<jintArray>(
        env->CallStaticObjectMethod(cls, mid, jPackageName, jPluginId, instanceId));
    env->DeleteLocalRef(jPackageName);
    env->DeleteLocalRef(jPluginId);
    env->DeleteLocalRef(cls);
    if (!result)
        return false;
    bool ok = false;
    if (env->GetArrayLength(result) >= 2) {
        jint values[2] = {};
        env->GetIntArrayRegion(result, 0, 2, values);
        if (values[0] > 0 && values[1] > 0) {
            width = values[0];
            height = values[1];
            ok = true;
        }
    }
    env->DeleteLocalRef(result);
    return ok;
}

} // namespace

void androidPixelsToWindowSize(int& width, int& height) {
    width = unscaledDimension(width);
    height = unscaledDimension(height);
}

bool queryRemoteViewPreferredSize(
    const char* pluginPackageName,
    const char* pluginId,
    int instanceId,
    int& width,
    int& height)
{
    auto* env = remidy::getAndroidJNIEnv();
    if (!env)
        return false;
    jint preferredWidth = 0;
    jint preferredHeight = 0;
    if (!callQueryRemoteViewPreferredSize(
            env,
            pluginPackageName,
            pluginId,
            static_cast<jint>(instanceId),
            preferredWidth,
            preferredHeight))
        return false;
    width = preferredWidth;
    height = preferredHeight;
    return true;
}

} // namespace remidy::gui::android

#endif
