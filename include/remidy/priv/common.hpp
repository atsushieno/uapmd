#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <thread>
#include <vector>
#if !WIN32
#include <pthread.h>
#endif

typedef uint32_t remidy_ump_t;
typedef int64_t remidy_timestamp_t;

#ifdef _MSC_VER
#define remidy_strcasecmp strcmpi
#else
#include <strings.h>
#define remidy_strcasecmp strcasecmp
#endif

namespace remidy {

    enum class StatusCode {
        OK,
        NOT_IMPLEMENTED,
        BUNDLE_NOT_FOUND,
        FAILED_TO_INSTANTIATE,
        ALREADY_INSTANTIATED,
        FAILED_TO_CONFIGURE,
        FAILED_TO_START_PROCESSING,
        FAILED_TO_STOP_PROCESSING,
        FAILED_TO_PROCESS,
        UNSUPPORTED_CHANNEL_LAYOUT_REQUESTED,
        ALREADY_INVALID_STATE,
        INVALID_PARAMETER_OPERATION,
        INSUFFICIENT_MEMORY
    };

    class Logger {
    public:
        class Impl;

#undef ERROR
        enum LogLevel {
            DIAGNOSTIC,
            INFO,
            WARNING,
            ERROR
        };

        static Logger* global();
        void logError(const char* format, ...);
        void logWarning(const char* format, ...);
        void logInfo(const char* format, ...);
        void logDiagnostic(const char* format, ...);
        static void stopDefaultLogger();

        Logger();
        ~Logger();

        void log(LogLevel level, const char* format, ...);
        void logv(LogLevel level, const char* format, va_list args);

        std::vector<std::function<void(LogLevel level, size_t serial, const char* s)>> callbacks;

    private:
        Impl *impl{nullptr};
    };

    // Facade to extension points in audio plugin abstraction layers such as
    // `AudioPluginFormat` and `AudioPluginInstance`.
    // Each extendable class implementors provide a derived class and provide
    // a getter that users of the extendable class can downcast to each class.
    // See how `AudioPluginFormatVST3::getExtensibility()` works for example.
    template <typename T>
    class PluginExtensibility {
        T& owner;
    protected:
        explicit PluginExtensibility(T& owner) : owner(owner) {
        }
        virtual ~PluginExtensibility() = default;
    };

    inline void setCurrentThreadNameIfPossible(std::string const threadName) {
#if __APPLE__
        pthread_setname_np(threadName.c_str());
#elif defined(__unix__) && !defined(__EMSCRIPTEN__)
        pthread_setname_np(pthread_self(), threadName.c_str());
#endif
    }

    std::vector<std::thread::id>& audioThreadIds();
}
