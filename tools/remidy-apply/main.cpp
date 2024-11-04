#include <atomic>
#include <iostream>
#include <ostream>
#include <ranges>

#include <cpptrace/from_current.hpp>
#include <cpplocate/cpplocate.h>
#include <remidy/remidy.hpp>
#include "PluginScanning.hpp"
#include "PluginInstancing.hpp"
#include <cxxopts.hpp>

// -------- instancing --------
const char* TOOLING_DIR_NAME= "remidy-tooling";

remidy_tooling::PluginScanning scanner{};

class RemidyApply {

int apply(std::string formatName, std::string pluginName, std::optional<std::string> audio, std::optional<std::string> smf, std::optional<std::string> smf2) {
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
    bool successful = false;
    {
        // scoped object
        remidy_tooling::PluginInstancing instancing{scanner, format, pluginInfo};
        // ...
        // you could adjust configuration here
        // ...
        instancing.makeAlive([&](std::string err) {
        });
        while (instancing.instancingState() == remidy_tooling::PluginInstancingState::Created ||
               instancing.instancingState()  == remidy_tooling::PluginInstancingState::Preparing)
            std::this_thread::yield();

        std::cerr << "  " << format->name() << ": Successfully configured " << displayName << ". Instantiating now..." << std::endl;

        instancing.withInstance([&](auto instance) {

            uint32_t numAudioIn = instance->audioInputBuses().size();
            uint32_t numAudioOut = instance->audioOutputBuses().size();
            remidy::AudioProcessContext ctx{4096};
            for (int32_t i = 0, n = numAudioIn; i < n; ++i)
                ctx.addAudioIn(instance->audioInputBuses()[i]->channelLayout().channels(), 1024);
            for (int32_t i = 0, n = numAudioOut; i < n; ++i)
                ctx.addAudioOut(instance->audioOutputBuses()[i]->channelLayout().channels(), 1024);
            ctx.frameCount(512);
            for (uint32_t i = 0; i < numAudioIn; i++) {
                // FIXME: channel count is not precise.
                memcpy(ctx.audioIn(i)->getFloatBufferForChannel(0), (void*) "0123456789ABCDEF", 16);
                memcpy(ctx.audioIn(i)->getFloatBufferForChannel(1), (void*) "FEDCBA9876543210", 16);
            }
            for (uint32_t i = 0; i < numAudioOut; i++) {
                // FIXME: channel count is not precise.
                memcpy(ctx.audioOut(i)->getFloatBufferForChannel(0), (void*) "02468ACE13579BDF", 16);
                memcpy(ctx.audioOut(i)->getFloatBufferForChannel(1), (void*) "FDB97531ECA86420", 16);
            }

            auto code = instance->process(ctx);
            if (code != remidy::StatusCode::OK)
                std::cerr << "  " << format->name() << ": " << displayName << " : process() failed. Error code " << (int32_t) code << std::endl;
            else
                successful = true;

        });
    }

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
        auto dir = cpplocate::localDir(TOOLING_DIR_NAME);
        auto pluginListCacheFile = dir.empty() ?  std::filesystem::path{""} : std::filesystem::path{dir}.append("plugin-list-cache.json");
        if (!std::filesystem::exists(pluginListCacheFile)) {
            std::cerr << "  remidy-apply needs existing plugin list cache first. Run `remidy-scan` first." << std::endl;
            return 1;
        }
        result = scanner.performPluginScanning(pluginListCacheFile);

        if (!parsedOpts.contains("p") || !parsedOpts.contains("f") || parsedOpts.contains("h")) {
            std::cerr << options.help();
            return parsedOpts.contains("h") ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        std::cerr << "Start testing instantiation... " << std::endl;

        std::thread thread([&] {
            CPPTRACE_TRY {
                result = apply(
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
