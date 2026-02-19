#include <iostream>
#include <thread>

#include <cpptrace/from_current.hpp>
#include <cxxopts.hpp>

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_SampleBuffers.h>

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

        // ---- Determine render duration ----
        double totalSeconds = 0.0;
        if (durationOverride.has_value()) {
            totalSeconds = *durationOverride;
        } else {
            auto snapshot = engine->timeline().buildMasterTrackSnapshot();
            if (!snapshot.empty())
                totalSeconds = snapshot.maxTimeSeconds;
        }

        if (totalSeconds <= 0.0) {
            std::cerr << "Cannot determine project duration. Use --duration to specify it." << std::endl;
            exitCode = EXIT_FAILURE;
            remidy::EventLoop::stop();
            return;
        }

        int64_t totalFrames = static_cast<int64_t>(std::ceil(totalSeconds * sampleRate));
        std::cerr << "Rendering " << totalSeconds << " s (" << totalFrames << " frames) at "
                  << sampleRate << " Hz..." << std::endl;

        // ---- Allocate device-level AudioProcessContext ----
        remidy::MasterContext masterContext;
        masterContext.sampleRate(sampleRate);
        masterContext.isPlaying(true);
        masterContext.playbackPositionSamples(0);

        remidy::AudioProcessContext deviceCtx(masterContext, DEFAULT_UMP_BUFFER_SIZE);
        deviceCtx.configureMainBus(OUTPUT_CHANNELS, OUTPUT_CHANNELS, bufferSize);

        // ---- Open WAV writer ----
        choc::audio::AudioFileProperties wavProps;
        wavProps.sampleRate   = static_cast<double>(sampleRate);
        wavProps.numChannels  = OUTPUT_CHANNELS;
        wavProps.bitDepth     = choc::audio::BitDepth::int16;

        auto writer = choc::audio::WAVAudioFileFormat<true>().createWriter(outPath, wavProps);
        if (!writer) {
            std::cerr << "Failed to open output file: " << outPath << std::endl;
            exitCode = EXIT_FAILURE;
            remidy::EventLoop::stop();
            return;
        }

        // ---- Render loop ----
        int64_t currentSample = 0;
        while (currentSample < totalFrames) {
            uint32_t framesToRender = static_cast<uint32_t>(
                std::min(static_cast<int64_t>(bufferSize), totalFrames - currentSample));

            deviceCtx.frameCount(framesToRender);
            masterContext.playbackPositionSamples(currentSample);

            // Clear output bus before processing
            for (uint32_t ch = 0; ch < OUTPUT_CHANNELS; ch++)
                memset(deviceCtx.getFloatOutBuffer(0, ch), 0, framesToRender * sizeof(float));

            engine->processAudio(deviceCtx);

            // Write output frames to WAV
            // Collect non-owning pointers to each channel's output buffer
            std::vector<const float*> channelPtrs(OUTPUT_CHANNELS);
            for (uint32_t ch = 0; ch < OUTPUT_CHANNELS; ch++)
                channelPtrs[ch] = deviceCtx.getFloatOutBuffer(0, ch);

            auto view = choc::buffer::createChannelArrayView(
                channelPtrs.data(),
                OUTPUT_CHANNELS,
                framesToRender);
            writer->appendFrames(view);

            currentSample += framesToRender;
        }

        writer->flush();
        std::cerr << "Done. Output written to: " << absolute(std::filesystem::path{outPath}) << std::endl;

        remidy::EventLoop::stop();
    });

    remidy::EventLoop::start();
    worker.join();
    return exitCode;
}

int main(int argc, const char* argv[]) {
    int result = EXIT_SUCCESS;
    CPPTRACE_TRY {
        result = run(argc, argv);
    } CPPTRACE_CATCH(const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        result = EXIT_FAILURE;
    }
    return result;
}
