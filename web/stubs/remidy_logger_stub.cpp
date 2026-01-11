#include <cstdarg>
#include <cstdio>
#include <vector>
#include <thread>
#include <remidy/priv/common.hpp>

namespace remidy {

static Logger g_logger; // simple global instance

Logger* Logger::global() { return &g_logger; }

Logger::Logger() : impl(nullptr) {}
Logger::~Logger() = default;

static void vlogf(const char* prefix, const char* fmt, va_list ap) {
    std::fprintf(stderr, "%s", prefix);
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
}

void Logger::logError(const char* format, ...) {
    va_list ap; va_start(ap, format); vlogf("[error] ", format, ap); va_end(ap);
}
void Logger::logWarning(const char* format, ...) {
    va_list ap; va_start(ap, format); vlogf("[warn]  ", format, ap); va_end(ap);
}
void Logger::logInfo(const char* format, ...) {
    va_list ap; va_start(ap, format); vlogf("[info]  ", format, ap); va_end(ap);
}
void Logger::logDiagnostic(const char* format, ...) {
    va_list ap; va_start(ap, format); vlogf("[diag]  ", format, ap); va_end(ap);
}

void Logger::stopDefaultLogger() {}

void Logger::log(LogLevel, const char* format, ...) {
    va_list ap; va_start(ap, format); vlogf("[log]   ", format, ap); va_end(ap);
}
void Logger::logv(LogLevel, const char* format, va_list args) {
    vlogf("[logv]  ", format, args);
}

} // namespace remidy

