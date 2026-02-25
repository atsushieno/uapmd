#include <thread>
#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#if UAPMD_HAS_CPPTRACE
#include <cpptrace/from_current.hpp>
#endif
#include <remidy/remidy.hpp>
#include <remidy-tooling/PluginScanTool.hpp>
#include <remidy-tooling/PluginInstancing.hpp>
#include <uapmd/uapmd.hpp>
#include <uapmd-engine/uapmd-engine.hpp>
#include <cxxopts.hpp>
#include <umppi/umppi.hpp>

// -------- instancing --------
remidy_tooling::PluginScanTool scanner{};

class RemidyApply {

    int direct_apply(remidy::PluginInstance* instance, std::optional<std::string> audio, std::optional<std::string> smf, std::optional<std::string> smf2) {
        bool successful = false;
        auto inputBuses = instance->audioBuses()->audioInputBuses();
        auto outputBuses = instance->audioBuses()->audioOutputBuses();

        uint32_t numAudioIn = inputBuses.size();
        uint32_t numAudioOut = outputBuses.size();
        remidy::MasterContext masterContext{};
        remidy::AudioProcessContext ctx{masterContext, 4096};
        ctx.configureMainBus(numAudioIn > 0 ? inputBuses[0]->channelLayout().channels() : 0,
                numAudioOut > 0 ? outputBuses[0]->channelLayout().channels() : 0,
                1024);
        /*
        for (int32_t i = 0, n = numAudioIn; i < n; ++i)
            ctx.addAudioIn(inputBuses[i]->channelLayout().channels(), 1024);
        for (int32_t i = 0, n = numAudioOut; i < n; ++i)
            ctx.addAudioOut(outputBuses[i]->channelLayout().channels(), 1024);
        */
        ctx.frameCount(512);
        for (size_t i = 0; i < numAudioIn; i++) {
            for (size_t ch = 0, nCh = ctx.inputChannelCount(i); ch < nCh; ch++)
                memcpy(ctx.getFloatInBuffer(i, ch), (void*) "0123456789ABCDEF", 16);
        }
        for (size_t i = 0; i < numAudioOut; i++) {
            // should not matter for audio processing though
            for (size_t ch = 0, nCh = ctx.outputChannelCount(i); ch < nCh; ch++)
                memcpy(ctx.getFloatOutBuffer(i, ch), (void*) "02468ACE13579BDF", 16);
        }

        auto code = instance->process(ctx);
        if (code != remidy::StatusCode::OK)
            std::cerr << "  " << instance->info()->format() << ": " << instance->info()->displayName() << " : process() failed. Error code " << (int32_t) code << std::endl;
        else
            successful = true;
        return successful ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    int direct_apply(std::string formatName, std::string pluginName, std::optional<std::string> audio, std::optional<std::string> smf, std::optional<std::string> smf2) {
        auto format = *(scanner.formats() | std::views::filter([&](auto fmt) { return fmt->name() == formatName; })).begin();
        if (!format) {
            std::cerr << "format not found: " << formatName << std::endl;
            return EXIT_FAILURE;
        }

        auto pluginInfo = *(scanner.catalog.getPlugins()
            | std::views::filter([&](auto e) { return e->format() == formatName && e->displayName() == pluginName; })).begin();
        if (!pluginInfo) {
            std::cerr << "plugin not found: " << pluginName << std::endl;
            return EXIT_FAILURE;
        }
        auto displayName = pluginInfo->displayName();

        if (!scanner.safeToInstantiate(format, pluginInfo)) {
            std::cerr << "  Plugin (" << formatName << ") " << displayName << " is known to not work..." << std::endl;
            return EXIT_FAILURE;
        }
        {
            // scoped object
            remidy_tooling::PluginInstancing instancing{scanner, format, pluginInfo};
            // ...
            // you could adjust configuration here
            // ...
            instancing.makeAlive([&](std::string err) {
                std::cerr << "makeAlive() completed." << std::endl;
            });
            while (instancing.instancingState() == remidy_tooling::PluginInstancingState::Created ||
                   instancing.instancingState()  == remidy_tooling::PluginInstancingState::Preparing)
                std::this_thread::yield();

            std::cerr << "  " << format->name() << ": Successfully configured " << displayName << ". Instantiating now..." << std::endl;

            instancing.withInstance([&](auto instance) {
                return direct_apply(instance, audio, smf, smf2);
            });
        }

        return EXIT_SUCCESS;
    }

    std::string uapmd_apply_get_plugin_id(std::string formatName, std::string pluginName) {
        auto format = *(scanner.formats() | std::views::filter([&](auto fmt) { return fmt->name() == formatName; })).begin();
        if (!format) {
            std::cerr << "format not found: " << formatName << std::endl;
            return "";
        }

        auto pluginInfo = *(scanner.catalog.getPlugins()
                            | std::views::filter([&](auto e) { return e->format() == formatName && e->displayName() == pluginName; })).begin();
        if (!pluginInfo) {
            std::cerr << "plugin not found: " << pluginName << std::endl;
            return "";
        }
        return pluginInfo->pluginId();
    }

    int uapmd_apply(std::string formatName, std::string pluginName, std::optional<std::string> audio, std::optional<std::string> smf, std::optional<std::string> smf2) {
        std::string pluginId = uapmd_apply_get_plugin_id(formatName, pluginName);
        if (pluginId.empty())
            return EXIT_FAILURE;

        // Configuration for offline rendering
        static constexpr uint32_t AUDIO_BUFFER_SIZE = 1024;
        static constexpr uint32_t UMP_BUFFER_SIZE = 65536;
        static constexpr int32_t SAMPLE_RATE = 48000;
        static constexpr int64_t TOTAL_BUFFERS = 200;  // ~4.2 seconds at 48kHz

        // For offline rendering, use SequencerEngine directly
        // (RealtimeSequencer requires a dispatcher)
        auto sequencer = uapmd::SequencerEngine::create(
            SAMPLE_RATE,
            AUDIO_BUFFER_SIZE,
            UMP_BUFFER_SIZE
        );

        // Add plugin track and wait for async instantiation
        std::atomic<bool> trackReady{false};
        uapmd::SequencerTrack* pluginTrack = nullptr;

        // We need to specify input/output channels for offline rendering
        uint32_t inputChannels = 2;   // Stereo input
        uint32_t outputChannels = 2;  // Stereo output

        // Set default channels for the sequencer
        sequencer->setDefaultChannels(inputChannels, outputChannels);

        auto trackIndex = sequencer->addEmptyTrack();
        sequencer->addPluginToTrack(trackIndex, formatName, pluginId,
            [&](int32_t instance, int32_t trackIndex, std::string error) {
                if (!error.empty()) {
                    std::cerr << "addSimplePluginTrack() failed: " << error << std::endl;
                    trackReady = true;
                    return;
                }
                pluginTrack = sequencer->tracks()[trackIndex];
                trackReady = true;
                std::cerr << "Plugin track loaded successfully" << std::endl;
            });

        // Wait for async track instantiation
        while (!trackReady)
            std::this_thread::yield();

        if (!pluginTrack) {
            std::cerr << "Failed to create plugin track" << std::endl;
            return EXIT_FAILURE;
        }

        // Get processing context and buffers
        auto& seqData = sequencer->data();
        auto& masterContext = seqData.masterContext();
        auto& tracks = seqData.tracks;

        if (tracks.empty()) {
            std::cerr << "No tracks available" << std::endl;
            return EXIT_FAILURE;
        }

        auto& processContext = *tracks[0];  // First track's AudioProcessContext

        // Configure transport
        masterContext.sampleRate(SAMPLE_RATE);
        masterContext.isPlaying(true);

        // Offline processing loop with sample-based timing
        int64_t currentSample = 0;
        constexpr int NUM_NOTES = 3;
        std::cerr << "Starting offline rendering..." << std::endl;

        for (int64_t bufferIndex = 0; bufferIndex < TOTAL_BUFFERS; bufferIndex++) {
            // Update playback position
            masterContext.playbackPositionSamples(currentSample);

            // Sample-based MIDI timing (instead of round-based)
            int64_t noteOnSample = 64 * AUDIO_BUFFER_SIZE;   // ~1.36 seconds
            int64_t noteOffSample = 128 * AUDIO_BUFFER_SIZE;  // ~2.73 seconds

            // Reset event input buffer
            processContext.eventIn().position(0);

            // Send MIDI events at specific sample positions
            if (currentSample == noteOnSample) {
                std::cerr << "Sending note-on at sample " << currentSample << std::endl;
                remidy_ump_t umpSequence[NUM_NOTES * 2];  // 2 UMP words per note-on
                for (int m = 0; m < NUM_NOTES; m++) {
                    uint64_t noteOn = umppi::UmpFactory::midi2NoteOn(0, 0, 72 + 4 * m, 0, 0xF800, 0);
                    umpSequence[m * 2] = noteOn >> 32;
                    umpSequence[m * 2 + 1] = noteOn & UINT32_MAX;
                }
                memcpy(processContext.eventIn().getMessages(), umpSequence, sizeof(umpSequence));
                processContext.eventIn().position(sizeof(umpSequence));
            }

            if (currentSample == noteOffSample) {
                std::cerr << "Sending note-off at sample " << currentSample << std::endl;
                remidy_ump_t umpSequence[NUM_NOTES * 2];  // 2 UMP words per note-off
                for (int m = 0; m < NUM_NOTES; m++) {
                    uint64_t noteOff = umppi::UmpFactory::midi2NoteOff(0, 0, 72 + 4 * m, 0, 0xF800, 0);
                    umpSequence[m * 2] = noteOff >> 32;
                    umpSequence[m * 2 + 1] = noteOff & UINT32_MAX;
                }
                memcpy(processContext.eventIn().getMessages(), umpSequence, sizeof(umpSequence));
                processContext.eventIn().position(sizeof(umpSequence));
            }

            // Prepare input audio buffers (silence for demo, could read from audio file)
            for (uint32_t bus = 0; bus < processContext.audioInBusCount(); bus++) {
                for (uint32_t ch = 0; ch < processContext.inputChannelCount(bus); ch++) {
                    float* buffer = processContext.getFloatInBuffer(bus, ch);
                    memset(buffer, 0, AUDIO_BUFFER_SIZE * sizeof(float));
                }
            }

            // Set frame count for this buffer
            processContext.frameCount(AUDIO_BUFFER_SIZE);

            // Process audio through all tracks (offline rendering)
            sequencer->processAudio(processContext);

            // Extract output buffers (write to audio file, analyze, etc.)
            // For now, just verify we're getting output
            for (uint32_t bus = 0; bus < processContext.audioOutBusCount(); bus++) {
                for (uint32_t ch = 0; ch < processContext.outputChannelCount(bus); ch++) {
                    float* buffer = processContext.getFloatOutBuffer(bus, ch);
                    // TODO: Write to output file or analyze
                    (void)buffer;  // Silence unused warning for demo
                }
            }

            currentSample += AUDIO_BUFFER_SIZE;
        }

        std::cerr << "Processing complete: " << TOTAL_BUFFERS << " buffers, "
                  << (currentSample / SAMPLE_RATE) << " seconds" << std::endl;

        return EXIT_SUCCESS;
    }

    public:
    int run(int argc, const char* const* argv) {
        int result{0};
        cxxopts::Options options("remidy-apply", "generate audio file from audio and MIDI inputs.");
        options.add_options()
            ("h,help", "print this help message")
            ("f,format", "Plugin format: `VST3`, `AU`, or `LV2`", cxxopts::value<std::string>())
            ("p,plugin", "The Plugin name to apply (`vendor: name` for more strict specification)", cxxopts::value<std::string>())
            ("a,audio", "Audio file to apply plugins", cxxopts::value<std::string>())
            ("m,midi", "MIDI 1.0 SMF to play", cxxopts::value<std::string>())
            ("m2,midi2", "MIDI 2.0 UMP clip file to play", cxxopts::value<std::string>())
            ("o,out", "Audio output file to save", cxxopts::value<std::string>())
        ;
        auto parsedOpts = options.parse(argc, argv);

        remidy::EventLoop::initializeOnUIThread();
        if (!std::filesystem::exists(scanner.pluginListCacheFile())) {
            std::cerr << "  remidy-apply needs existing plugin list cache first. Run `remidy-scan` first." << std::endl;
            return 1;
        }
        result = scanner.performPluginScanning(false);

        if (!parsedOpts.contains("p") || !parsedOpts.contains("f") || parsedOpts.contains("h")) {
            std::cerr << options.help();
            return parsedOpts.contains("h") ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        std::cerr << "Start testing instantiation... " << std::endl;

        std::thread thread([&] {
            result = uapmd_apply(
                parsedOpts["f"].as<std::string>(),
                parsedOpts["p"].as<std::string>(),
                parsedOpts["a"].as_optional<std::string>(),
                parsedOpts["m"].as_optional<std::string>(),
                parsedOpts["m2"].as_optional<std::string>());
            remidy::EventLoop::stop();

            std::cerr << "Completed " << std::endl;
        });
        remidy::EventLoop::start();
        return result;
    }
};

int main(int argc, const char* argv[]) {
    auto runApply = [&](int argcValue, const char* const* argvValue) {
        RemidyApply apply{};
        return apply.run(argcValue, argvValue);
    };
#if UAPMD_HAS_CPPTRACE
    int result{0};
    CPPTRACE_TRY {
        result = runApply(argc, argv);
    } CPPTRACE_CATCH(const std::exception& e) {
        std::cerr << "Exception in testCreateInstance: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        result = EXIT_FAILURE;
    }
    return result;
#else
    try {
        return runApply(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Exception in testCreateInstance: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
#endif
}
