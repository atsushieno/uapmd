#if defined(_WIN32)
#include <windows.h>
#include <processthreadsapi.h>
#elif __APPLE__ || defined(__unix__) && !defined(__EMSCRIPTEN__)
#include <pthread.h>
#endif
#include "remidy/detail/common.hpp"

void remidy::setCurrentThreadNameIfPossible(const std::string& threadName) {
#if defined(_WIN32)
    std::wstring s = std::wstring(threadName.begin(), threadName.end());
    SetThreadDescription(GetCurrentThread(), s.c_str());
#elif __APPLE__
    pthread_setname_np(threadName.c_str());
#elif defined(__unix__) && !defined(__EMSCRIPTEN__)
    pthread_setname_np(pthread_self(), threadName.c_str());
#endif
}
