#include <iostream>
#include <thread>
#include <format>
#include <filesystem>
#include <optional>

#if UAPMD_HAS_CPPTRACE
#include <cpptrace/from_current.hpp>
#endif
#include <cxxopts.hpp>

#include <remidy/remidy.hpp>
#include <uapmd/uapmd.hpp>
#include <uapmd-engine/uapmd-engine.hpp>

static constexpr uint32_t DEFAULT_AUDIO_BUFFER_SIZE = 1024;
static constexpr uint32_t DEFAULT_UMP_BUFFER_SIZE = 65536;
static constexpr int32_t  DEFAULT_SAMPLE_RATE      = 48000;
static constexpr uint32_t OUTPUT_CHANNELS          = 2;

int run(int argc, const char* argv[]) {
    cxxopts::Options options("uapmd-apply", "Render a uapmd project file to an audio file.");
    options.positional_help("<project.uapmd>");
    options.show_positional_help();
    options.add_options()
        ("h,help",        "Print this help message")
        ("project",       "Path to the .uapmd project file", cxxopts::value<std::string>())
        ("o,out",         "Output WAV file path",
                          cxxopts::value<std::string>()->default_value("rendered.wav"))
        ("r,sample-rate", "Sample rate in Hz",
                          cxxopts::value<int32_t>()->default_value(std::to_string(DEFAULT_SAMPLE_RATE)))
        ("b,buffer-size", "Audio buffer size in frames",
                          cxxopts::value<uint32_t>()->default_value(std::to_string(DEFAULT_AUDIO_BUFFER_SIZE)))
        ("d,duration",    "Override render duration in seconds (default: derived from project)",
                          cxxopts::value<double>())
    ;
    options.parse_positional({"project"});

    auto opts = options.parse(argc, argv);

    if (opts.contains("h") || !opts.contains("project")) {
        std::cerr << options.help();
        return opts.contains("h") ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    auto projectPath   = opts["project"].as<std::string>();
    auto outPath       = opts["out"].as<std::string>();
    auto sampleRate    = opts["sample-rate"].as<int32_t>();
    auto bufferSize    = opts["buffer-size"].as<uint32_t>();
    std::optional<double> durationOverride;
    if (opts.contains("d"))
        durationOverride = opts["d"].as<double>();

    if (!std::filesystem::exists(projectPath)) {
        std::cerr << "Project file not found: " << projectPath << std::endl;
        return EXIT_FAILURE;
    }

    int exitCode = EXIT_SUCCESS;

    remidy::EventLoop::initializeOnUIThread();

    std::thread worker([&] {
        // ---- Create engine ----
        auto engine = uapmd::SequencerEngine::create(sampleRate, bufferSize, DEFAULT_UMP_BUFFER_SIZE);
        engine->offlineRendering(true);
        engine->setDefaultChannels(OUTPUT_CHANNELS, OUTPUT_CHANNELS);

        // ---- Load project (blocks until all plugins are instantiated) ----
        std::cerr << "Loading project: " << projectPath << std::endl;

        auto result = engine->timeline().loadProject(projectPath);

        if (!result.success) {
            std::cerr << "Failed to load project: " << result.error << std::endl;
            exitCode = EXIT_FAILURE;
            remidy::EventLoop::stop();
            return;
        }

        std::cerr << "Project loaded. " << engine->tracks().size() << " track(s)." << std::endl;

        auto bounds = engine->timeline().calculateContentBounds();

        uapmd::OfflineRenderSettings renderSettings{};
        renderSettings.outputPath = outPath;
        renderSettings.sampleRate = sampleRate;
        renderSettings.bufferSize = bufferSize;
        renderSettings.outputChannels = OUTPUT_CHANNELS;
        renderSettings.umpBufferSize = DEFAULT_UMP_BUFFER_SIZE;
        renderSettings.startSeconds = bounds.hasContent ? bounds.firstSeconds : 0.0;
        renderSettings.contentBoundsValid = bounds.hasContent;
        renderSettings.contentStartSeconds = bounds.firstSeconds;
        renderSettings.contentEndSeconds = bounds.lastSeconds;
        renderSettings.useContentFallback = !durationOverride.has_value();
        if (durationOverride.has_value()) {
            renderSettings.endSeconds = renderSettings.startSeconds + std::max(0.0, *durationOverride);
        } else if (!bounds.hasContent) {
            std::cerr << "Cannot determine project duration. Use --duration to specify it." << std::endl;
            exitCode = EXIT_FAILURE;
            remidy::EventLoop::stop();
            return;
        }

        std::cerr << "Rendering to " << outPath << " ..." << std::endl;

        uapmd::OfflineRenderCallbacks callbacks{};
        callbacks.onProgress = [](const uapmd::OfflineRenderProgress& progress) {
            std::cerr << std::format(
                "\rProgress: {0:.2f}s / {1:.2f}s ({2:.1f}%)",
                progress.renderedSeconds,
                progress.totalSeconds,
                progress.progress * 100.0);
            std::cerr.flush();
        };
        callbacks.shouldCancel = []() { return false; };

        auto renderResult = uapmd::renderOfflineProject(*engine, renderSettings, callbacks);
        std::cerr << std::endl;

        if (renderResult.canceled) {
            std::cerr << "Render canceled." << std::endl;
            exitCode = EXIT_FAILURE;
            remidy::EventLoop::stop();
            return;
        }

        if (!renderResult.success) {
            std::string error = renderResult.errorMessage.empty()
                ? "Render failed."
                : renderResult.errorMessage;
            if (!bounds.hasContent && !durationOverride.has_value())
                error += " (Try passing --duration when the project has no content bounds.)";
            std::cerr << error << std::endl;
            exitCode = EXIT_FAILURE;
            remidy::EventLoop::stop();
            return;
        }

        std::cerr << "Done. Output written to: "
                  << absolute(std::filesystem::path{outPath})
                  << " (" << renderResult.renderedSeconds << " s)" << std::endl;

        remidy::EventLoop::stop();
    });

    remidy::EventLoop::start();
    worker.join();
    return exitCode;
}

int main(int argc, const char* argv[]) {
#if UAPMD_HAS_CPPTRACE
    int result = EXIT_SUCCESS;
    CPPTRACE_TRY {
        result = run(argc, argv);
    } CPPTRACE_CATCH(const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        result = EXIT_FAILURE;
    }
    return result;
#else
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
#endif
}
