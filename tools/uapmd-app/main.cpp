// Desktop entry point for UAPMD
// For Android, see android_main.cpp
// Shared application logic is in main_common.cpp

#include "main_common.hpp"
#include <cpptrace/cpptrace.hpp>
#include <cpptrace/from_current.hpp>
#include <iostream>
#include <cstring>

#include "cpptrace/utils.hpp"

#if defined(__unix__) || defined(__APPLE__)
    #include <signal.h>
    #include <unistd.h>
#endif

namespace {
#if defined(__unix__) || defined(__APPLE__)
[[noreturn]] void bestEffortSegvHandler(int signo, siginfo_t* info, void* context) {
    (void)signo;
    (void)info;
    (void)context;
    constexpr char header[] = "uapmd-app caught SIGSEGV (best-effort stack trace):\n";
    write(STDERR_FILENO, header, sizeof(header) - 1);
    try {
        cpptrace::generate_trace().print();
    } catch (...) {
        constexpr char failure[] = "cpptrace failed while printing stack trace from handler.\n";
        write(STDERR_FILENO, failure, sizeof(failure) - 1);
    }
    _Exit(1);
}

void installSignalTraceHandler() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = &bestEffortSegvHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, nullptr) != 0) {
        std::perror("sigaction");
    }
}
#else
void installSignalTraceHandler() {}
#endif
} // namespace

// Desktop-specific signal handlers and crash reporting

/**
 * Desktop entry point with exception handling and crash reporting.
 */
int main(int argc, char** argv) {
    cpptrace::register_terminate_handler();
    installSignalTraceHandler();
    int ret;
    CPPTRACE_TRY {
        ret = uapmd::runMainLoop(argc, argv);
    } CPPTRACE_CATCH(...) {
        std::cerr << "Runtime Error in uapmd-app: " << std::endl;
        cpptrace::from_current_exception().print();
        ret = EXIT_FAILURE;
    }
    return ret;
}
