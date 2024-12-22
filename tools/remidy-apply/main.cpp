#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#include <cpptrace/from_current.hpp>
#include <remidy/remidy.hpp>
#include <remidy-tooling/PluginScanning.hpp>
#include <remidy-tooling/PluginInstancing.hpp>
#include <uapmd/uapmd.hpp>
#include <cxxopts.hpp>
#include <cmidi2.h>

// -------- instancing --------
remidy_tooling::PluginScanning scanner{};

class RemidyApply {

    int direct_apply(remidy::PluginInstance* instance, std::optional<std::string> audio, std::optional<std::string> smf, std::optional<std::string> smf2) {
        bool successful = false;
        auto inputBuses = instance->audioBuses()->audioInputBuses();
        auto outputBuses = instance->audioBuses()->audioOutputBuses();

        uint32_t numAudioIn = inputBuses.size();
        uint32_t numAudioOut = outputBuses.size();
        remidy::MasterContext masterContext{};
        remidy::TrackContext trackContext{masterContext};
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
        auto format = *(scanner.formats | std::views::filter([&](auto fmt) { return fmt->name() == formatName; })).begin();
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
        auto format = *(scanner.formats | std::views::filter([&](auto fmt) { return fmt->name() == formatName; })).begin();
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

        std::atomic playing{true};

        static uint32_t AUDIO_BUFFER_SIZE = 1024;
        static uint32_t UMP_BUFFER_SIZE = 65536;
        auto dispatcher = std::make_unique<uapmd::DeviceIODispatcher>(UMP_BUFFER_SIZE);
        auto sequencer = std::make_unique<uapmd::SequenceProcessor>(dispatcher->audioDriver()->sampleRate(), AUDIO_BUFFER_SIZE, UMP_BUFFER_SIZE);
        sequencer->addSimpleTrack(formatName, pluginId, [&](uapmd::AudioPluginTrack* track, std::string error) {
            if (!error.empty()) {
                std::cerr << "addSimpleTrack() failed." << std::endl;
                playing = false;
                return;
            }
            uint32_t round = 0;
            remidy_ump_t umpSequence[512];
            remidy::AudioProcessContext process{sequencer->data().masterContext(), UMP_BUFFER_SIZE};
            // FIXME: provide correct channel numbers by main bus
            process.configureMainBus(dispatcher->audioDriver()->channels(), dispatcher->audioDriver()->channels(), 1024);

            dispatcher->addCallback([&](remidy::AudioProcessContext& data) {
                for (auto track : sequencer->tracks()) {
                    process.eventIn().position(0);
                    // demo MIDI input buffers (some notes, one shot)
                    memset(umpSequence, 0, sizeof(remidy_ump_t) * 512);
                    int numNotes = 3;
                    switch (round++) {
                        case 64: {
                            for (int m = 0; m < numNotes; m++) {
                                int64_t noteOn = cmidi2_ump_midi2_note_on(0, 0, 72 + 4 * m, 0, 0xF800, 0);
                                umpSequence[m * 2] = noteOn >> 32;
                                umpSequence[m * 2 + 1] = noteOn & UINT32_MAX;
                            }
                            memcpy(process.eventIn().getMessages(), umpSequence, 8 * numNotes);
                            process.eventIn().position(8 * numNotes);
                            break;
                        }
                        case 128: {
                            for (int m = 0; m < numNotes; m++) {
                                int64_t noteOff = cmidi2_ump_midi2_note_off(0, 0, 72 + 4 * m, 0, 0xF800, 0);
                                umpSequence[m * 2] = noteOff >> 32;
                                umpSequence[m * 2 + 1] = noteOff & UINT32_MAX;
                            }
                            memcpy(process.eventIn().getMessages(), umpSequence, 8 * numNotes);
                            process.eventIn().position(8 * numNotes);
                            break;
                        }
                    }

                    // FIXME: avoid memcpy (audio input)
                    for (auto bus = 0, n = data.audioInBusCount(); bus < n; bus++) {
                        for (uint32_t ch = 0, nCh = data.inputChannelCount(bus); ch < nCh; ch++)
                            memcpy(process.getFloatInBuffer(bus, ch),
                                   data.getFloatInBuffer(bus, ch),
                                   data.frameCount() * sizeof(float));
                    }
                    process.frameCount(data.frameCount());

                    if (auto ret = track->processAudio(process); ret)
                        return ret;

                    process.eventIn().position(0); // reset

                    // FIXME: avoid memcpy (audio output)
                    for (auto bus = 0, n = process.audioOutBusCount(); bus < n; bus++) {
                        for (uint32_t ch = 0, nCh = process.outputChannelCount(bus); ch < nCh; ch++)
                            memcpy(data.getFloatOutBuffer(bus, ch),
                                   process.getFloatOutBuffer(bus, ch),
                                   data.frameCount() * sizeof(float));
                    }

                    // FIXME: collect MIDI output

                }

                // FIXME: define status codes
                return 0;
            });
            dispatcher->start();

            std::cerr << "Type [CR] to quit." << std::endl;
            std::cin.get();
            dispatcher->stop();

            playing = false;
        });
        while (playing)
            std::this_thread::yield();

        return EXIT_SUCCESS;
    }

    public:
    int run(int argc, const char* argv[]) {
        int result{0};
        CPPTRACE_TRY {
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
            result = scanner.performPluginScanning();

            if (!parsedOpts.contains("p") || !parsedOpts.contains("f") || parsedOpts.contains("h")) {
                std::cerr << options.help();
                return parsedOpts.contains("h") ? EXIT_SUCCESS : EXIT_FAILURE;
            }

            std::cerr << "Start testing instantiation... " << std::endl;

            std::thread thread([&] {
                CPPTRACE_TRY {
                    result = uapmd_apply(
                        parsedOpts["f"].as<std::string>(),
                        parsedOpts["p"].as<std::string>(),
                        parsedOpts["a"].as_optional<std::string>(),
                        parsedOpts["m"].as_optional<std::string>(),
                        parsedOpts["m2"].as_optional<std::string>());
                    remidy::EventLoop::stop();

                    std::cerr << "Completed " << std::endl;
                } CPPTRACE_CATCH(const std::exception& e) {
                    std::cerr << "Exception in main: " << e.what() << std::endl;
                    cpptrace::from_current_exception().print();
                }
            });
            remidy::EventLoop::start();
        } CPPTRACE_CATCH(const std::exception& e) {
            std::cerr << "Exception in testCreateInstance: " << e.what() << std::endl;
            cpptrace::from_current_exception().print();
        }
        return result;
    }
};

int main(int argc, const char* argv[]) {
    RemidyApply apply{};
    apply.run(argc, argv);
}
