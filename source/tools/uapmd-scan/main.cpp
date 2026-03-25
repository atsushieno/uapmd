#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#if UAPMD_HAS_CPPTRACE
#include <cpptrace/from_current.hpp>
#endif

#include <../../remidy-tooling/include/remidy-tooling/priv/ScanOnlyRunner.hpp>
#include <remidy-tooling/remidy-tooling.hpp>

namespace {

constexpr double kDefaultTimeoutSeconds = 120.0;

void printHelp(const char* programName) {
    std::cout << "Usage: " << programName << " [options]\n"
              << "Perform a full plugin scan plus verification (equivalent to uapmd-app --scan-only --full --force-rescan).\n\n"
              << "Options:\n"
              << "  -h, --help            Show this help message and exit\n"
              << "      --timeout SEC     Abort a hung plugin bundle after SEC seconds (default 120)\n";
}

struct CommandLineOptions {
    bool showHelp = false;
    bool valid = true;
    double timeoutSeconds = kDefaultTimeoutSeconds;
};

bool parseTimeout(std::string_view value, double& timeoutSeconds) {
    try {
        double parsed = std::stod(std::string(value));
        if (parsed < 0.0 || !std::isfinite(parsed))
            return false;
        timeoutSeconds = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

CommandLineOptions parseOptions(int argc, char** argv) {
    CommandLineOptions opts{};
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h" || arg == "-?") {
            opts.showHelp = true;
            continue;
        }
        if (arg == "--timeout") {
            if (i + 1 >= argc) {
                std::cerr << "--timeout requires a value\n";
                opts.valid = false;
                break;
            }
            std::string_view value{argv[++i]};
            if (!parseTimeout(value, opts.timeoutSeconds)) {
                std::cerr << "Invalid timeout value: " << value << "\n";
                opts.valid = false;
                break;
            }
            continue;
        }
        std::cerr << "Unknown argument: " << arg << "\n";
        opts.valid = false;
        break;
    }
    return opts;
}

int runScanner(int argc, char** argv) {
    remidy_tooling::RemotePluginScannerProcess remoteScanner(argc, argv);
    if (remoteScanner.matches())
        return remoteScanner.process();

    auto cli = parseOptions(argc, argv);
    if (!cli.valid) {
        std::cerr << "Use --help to see available options." << std::endl;
        return EXIT_FAILURE;
    }

    if (cli.showHelp) {
        printHelp(argc > 0 ? argv[0] : "uapmd-scan");
        return EXIT_SUCCESS;
    }

    remidy_tooling::ScanOnlyOptions options{};
    options.forceRescan = true;
    options.fullVerification = true;
    options.useRemoteScanner = true;
    options.bundleTimeoutSeconds = cli.timeoutSeconds;

    return remidy_tooling::runScanOnlyMode(options);
}

}

int main(int argc, char** argv) {
#if UAPMD_HAS_CPPTRACE
    int result = EXIT_SUCCESS;
    CPPTRACE_TRY {
        result = runScanner(argc, argv);
    } CPPTRACE_CATCH(const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        result = EXIT_FAILURE;
    }
    return result;
#else
    try {
        return runScanner(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
#endif
}
