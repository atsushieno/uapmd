#include <iostream>
#include <remidy.hpp>
#include <rtlog/rtlog.h>

constexpr auto MAX_NUM_LOG_MESSAGES = 128;
constexpr auto MAX_LOG_MESSAGE_LENGTH = 1024;

static std::atomic<std::size_t> log_serial{ 0 };

using RealtimeLogger = rtlog::Logger<remidy::Logger::LogMetadata, MAX_NUM_LOG_MESSAGES, MAX_LOG_MESSAGE_LENGTH, log_serial>;
static RealtimeLogger logger;

void remidy::Logger::log(LogMetadata data, const char *format, ...) {
    for (auto& func : callbacks) {
        va_list args;
        va_start(args, format);
        logger.Log(std::move(data), format, args);
        va_end(args);
    }
}

#define REMIDY_LOGGER_FUNC(LEVEL, format) \
    va_list args; \
    va_start(args, format); \
    Logger::getGlobal()->log({LEVEL}, format, args); \
    va_end(args); \

void remidy::Logger::defaultLogError(const char *format, ...) {
    REMIDY_LOGGER_FUNC(Logger::LogLevel::ERROR, format)
}
void remidy::Logger::defaultLogWarning(const char *format, ...) {
    REMIDY_LOGGER_FUNC(Logger::LogLevel::WARNING, format)
}
void remidy::Logger::defaultLogInfo(const char *format, ...) {
    REMIDY_LOGGER_FUNC(Logger::LogLevel::INFO, format)
}
void remidy::Logger::defaultLogDiagnostic(const char *format, ...) {
    REMIDY_LOGGER_FUNC(Logger::LogLevel::DIAGNOSTIC, format)
}

class PrintMessageFunctor
{
public:
    PrintMessageFunctor() = default;
    PrintMessageFunctor(const PrintMessageFunctor&) = delete;
    PrintMessageFunctor(PrintMessageFunctor&&) = delete;
    PrintMessageFunctor& operator=(const PrintMessageFunctor&) = delete;
    PrintMessageFunctor& operator=(PrintMessageFunctor&&) = delete;

    static const char* levelString(remidy::Logger::LogLevel level) {
        switch (level) {
            case remidy::Logger::LogLevel::INFO: return "I";
            case remidy::Logger::LogLevel::WARNING: return "W";
            case remidy::Logger::LogLevel::ERROR: return "E";
            case remidy::Logger::LogLevel::DIAGNOSTIC: return "D";
        }
        return "";
    }

    void operator()(const remidy::Logger::LogMetadata& data, size_t serial, const char* fstring, ...) __attribute__ ((format (printf, 4, 5)))
    {
        std::array<char, MAX_LOG_MESSAGE_LENGTH> buffer;

        va_list args;
        va_start(args, fstring);
        vsnprintf(buffer.data(), buffer.size(), fstring, args);
        va_end(args);

        std::cerr << "[remidy #" << serial << " (" << levelString(data.level) << ")]: " << buffer.data() << std::endl;
    }
};

static PrintMessageFunctor PrintMessage;

rtlog::LogProcessingThread<RealtimeLogger, PrintMessageFunctor>* getLaunchedLoggerThread() {
    static rtlog::LogProcessingThread thread(logger, PrintMessage, std::chrono::milliseconds(10));
    return &thread;
}

remidy::Logger * remidy::Logger::getGlobal() {
    static Logger instance{};
    static bool loggerInitialized{false};

    if (!loggerInitialized) {
        getLaunchedLoggerThread(); // launch it by static member
        instance.callbacks.emplace_back([](remidy::Logger::LogMetadata data, const char* fstring, ...) {
            va_list args;
            va_start(args, fstring);
            PrintMessage(data, log_serial++, fstring, args);
            va_end(args);
        });
        loggerInitialized = true;
    }

    return &instance;
}

void remidy::Logger::stopDefaultLogger() {
    getLaunchedLoggerThread()->Stop();
}
