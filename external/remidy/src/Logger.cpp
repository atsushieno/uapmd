#include <iostream>
#include <remidy.hpp>
#include <rtlog/rtlog.h>

constexpr auto MAX_NUM_LOG_MESSAGES = 128;
constexpr auto MAX_LOG_MESSAGE_LENGTH = 1024;

static std::atomic<std::size_t> log_serial{ 0 };

struct LogContext {
    remidy::Logger::LogLevel level;
    const remidy::Logger* owner;
    const void* logger;
};

using RealtimeLogger = rtlog::Logger<LogContext, MAX_NUM_LOG_MESSAGES, MAX_LOG_MESSAGE_LENGTH, log_serial>;
static RealtimeLogger rt_logger;

void remidy::Logger::log(LogLevel level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    logv(level, format, args);
    va_end(args);
}

void remidy::Logger::logv(LogLevel level, const char *format, va_list args) {
    rt_logger.Logv(LogContext{.level = level, .owner = this, .logger = &rt_logger}, format, args);
}

#define DEFINE_DEFAULT_LOGGER(UPPER, CAMEL) \
void remidy::Logger::defaultLog##CAMEL(const char *format, ...) { \
    va_list args; \
    va_start(args, format); \
    remidy::Logger::getGlobal()->logv(UPPER, format, args); \
    va_end(args); \
}

DEFINE_DEFAULT_LOGGER(ERROR , Error)
DEFINE_DEFAULT_LOGGER(WARNING , Warning)
DEFINE_DEFAULT_LOGGER(INFO , Info)
DEFINE_DEFAULT_LOGGER(DIAGNOSTIC , Diagnostic)

class CallbackMessageFunctor
{
public:
    CallbackMessageFunctor() = default;
    CallbackMessageFunctor(const CallbackMessageFunctor&) = delete;
    CallbackMessageFunctor(CallbackMessageFunctor&&) = delete;
    CallbackMessageFunctor& operator=(const CallbackMessageFunctor&) = delete;
    CallbackMessageFunctor& operator=(CallbackMessageFunctor&&) = delete;

    void operator()(const LogContext& data, size_t serial, const char* format, ...) __attribute__ ((format (printf, 4, 5))) {
        std::array<char, MAX_LOG_MESSAGE_LENGTH> buffer;

        va_list args;
        va_start(args, format);
        vsnprintf(buffer.data(), buffer.size(), format, args);
        va_end(args);
        for (auto& func : data.owner->callbacks) {
            func(data.level, serial, buffer.data());
        }
    }
};

static CallbackMessageFunctor ForwardToCallbacks;

rtlog::LogProcessingThread<RealtimeLogger, CallbackMessageFunctor>* getLaunchedLoggerThread() {
    static rtlog::LogProcessingThread thread(rt_logger, ForwardToCallbacks, std::chrono::milliseconds(10));
    return &thread;
}

static const char* levelString(remidy::Logger::LogLevel level) {
    switch (level) {
        case remidy::Logger::LogLevel::INFO: return "I";
        case remidy::Logger::LogLevel::WARNING: return "W";
        case remidy::Logger::LogLevel::ERROR: return "E";
        case remidy::Logger::LogLevel::DIAGNOSTIC: return "D";
    }
    return "";
}

remidy::Logger * remidy::Logger::getGlobal() {
    static Logger instance{};
    static bool loggerInitialized{false};

    if (!loggerInitialized) {
        getLaunchedLoggerThread(); // launch it by static member
        instance.callbacks.emplace_back([](remidy::Logger::LogLevel level, size_t serial, const char* fstring, ...) {
            std::array<char, MAX_LOG_MESSAGE_LENGTH> buffer;

            va_list args;
            va_start(args, fstring);
            vsnprintf(buffer.data(), buffer.size(), fstring, args);
            va_end(args);

            std::cerr << "[remidy-global #" << serial << " (" << levelString(level) << ")]: " << buffer.data() << std::endl;
        });
        loggerInitialized = true;
    }

    return &instance;
}

void remidy::Logger::stopDefaultLogger() {
    getLaunchedLoggerThread()->Stop();
}
