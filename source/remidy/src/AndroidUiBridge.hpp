#pragma once

#include <functional>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace remidy {

#if defined(__ANDROID__)
void initAndroidUiBridge(JNIEnv* env, jobject activity);
#endif
void runOnAndroidUiThread(std::function<void()> task);
void runOnAndroidUiThreadSync(std::function<void()> task);

} // namespace remidy
