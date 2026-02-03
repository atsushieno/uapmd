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

using RealtimeLogger = rtlog::Logger<LogContext, MAX_NUM_LOG_MESSAGES, MAX_LOG_MESSAGE_LENGTH, log_serial, rtlog::MultiRealtimeWriterQueueType>;
static RealtimeLogger rt_logger;

void remidy::Logger::log(LogLevel level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    logv(level, format, args);
    va_end(args);
}

class CallbackMessageFunctor
{
public:
    CallbackMessageFunctor() = default;
    CallbackMessageFunctor(const CallbackMessageFunctor&) = delete;
    CallbackMessageFunctor(CallbackMessageFunctor&&) = delete;
    CallbackMessageFunctor& operator=(const CallbackMessageFunctor&) = delete;
    CallbackMessageFunctor& operator=(CallbackMessageFunctor&&) = delete;

#if WIN32
    void operator()(const LogContext& data, size_t serial, const char* format, ...)
#else
    void operator()(const LogContext& data, size_t serial, const char* format, ...) __attribute__ ((format (printf, 4, 5)))
#endif
    {
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

class remidy::Logger::Impl {
    Logger* owner;

public:
    explicit Impl(Logger* owner) :
        owner(owner) {
        initializeGlobalLogger();
    }
    ~Impl() {
        getLaunchedLoggerThread()->Stop();
    }

    void initializeGlobalLogger();

    void log(LogLevel level, const char *format, ...) {
        va_list args;
        va_start(args, format);
        logv(level, format, args);
        va_end(args);
    }

    void logv(LogLevel level, const char *format, va_list args) {
        rt_logger.Logv(LogContext{.level = level, .owner = owner, .logger = &rt_logger}, format, args);
    }
};


remidy::Logger::Logger() {
    impl = new Impl(this);
}

remidy::Logger::~Logger() {
    delete impl;
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

void remidy::Logger::Impl::initializeGlobalLogger() {
    static std::atomic<bool> loggerInitialized{false};

    if (!loggerInitialized.exchange(true)) {
        owner->callbacks.emplace_back([](remidy::Logger::LogLevel level, size_t serial, const char* s) {
            switch (level) {
                // too much by default
                case LogLevel::DIAGNOSTIC: break;
                default:
                    std::cerr << "[remidy #" << serial << " (" << levelString(level) << ")]: " << s << std::endl;
                    break;
            }
        });
        getLaunchedLoggerThread();
    }
}

void remidy::Logger::logv(LogLevel level, const char *format, va_list args) {
    impl->logv(level, format, args);
}

void remidy::Logger::stopDefaultLogger() {
    getLaunchedLoggerThread()->Stop();
}

#define DEFINE_DEFAULT_LOGGER(UPPER, CAMEL) \
void remidy::Logger::log##CAMEL(const char *format, ...) { \
va_list args; \
va_start(args, format); \
impl->logv(UPPER, format, args); \
va_end(args); \
}

DEFINE_DEFAULT_LOGGER(ERROR , Error)
DEFINE_DEFAULT_LOGGER(WARNING , Warning)
DEFINE_DEFAULT_LOGGER(INFO , Info)
DEFINE_DEFAULT_LOGGER(DIAGNOSTIC , Diagnostic)


static remidy::Logger instance{};
remidy::Logger* remidy::Logger::global() {
    return &instance;
}
