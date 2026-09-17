#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "AudioPluginCatalog.hpp"
#include "AudioPluginScanning.hpp"
#include "AudioPluginInstanceAPI.hpp"

namespace uapmd_plugin_hosting {

    // Which plugin operations have to happen on the UI/main thread. [flags]
    //
    // VST3 and CLAP require almost every plugin operation on the main thread, while AU and
    // LV2 do not. This costs the application a lot of parallelism, so a format may report a
    // narrower requirement, and may report it per plugin when only some of its plugins
    // misbehave.
    enum AudioPluginUIThreadRequirement : uint32_t {
        UIThreadNotRequired = 0,
        UIThreadForInstanceControl = 1,
        UIThreadForParameters = 2,
        UIThreadForState = 4,
        UIThreadForAllNonAudioOperation = 0xFFFFFFFF
    };

    struct AudioPluginInstantiationOptions {
        AudioPluginUIThreadRequirement uiThreadRequirement{UIThreadNotRequired};
        uint32_t sampleRate{48000};
        uint32_t bufferSizeInSamples{4096};
        bool offlineMode{false};
        std::optional<uint32_t> mainInputChannels{};
        std::optional<uint32_t> mainOutputChannels{};
    };

    // A plugin format that this host can scan and instantiate.
    //
    // This is the abstraction the rest of uapmd-plugin-hosting is written against, and it is
    // what an application implements to add a plugin format of its own (JSFX, SFZ, and so
    // on). The formats we ship are implemented over remidy, but nothing here depends on
    // remidy, so an implementation does not have to go through it.
    //
    // Register an implementation with PluginScanTool::addFormat() or
    // AudioPluginHostingAPI::addPluginFormat().
    class AudioPluginFormat {
    public:
        virtual ~AudioPluginFormat() = default;

        // The format name. It identifies the format everywhere: it is what
        // AudioPluginCatalogEntry::format() holds, and what callers pass to
        // AudioPluginHostingAPI::createPluginInstance(). It must be unique among the
        // registered formats.
        virtual std::string name() = 0;

        // Which operations on this plugin have to run on the UI/main thread.
        virtual AudioPluginUIThreadRequirement requiresUIThreadOn(AudioPluginCatalogEntry* entry) = 0;

        // How this format discovers installed plugins. Returning null disables scanning,
        // which only makes sense for a format whose plugin list comes from somewhere else.
        virtual AudioPluginScanning* scanning() = 0;

        // Asynchronously creates a plugin instance, already configured for `options` and
        // ready to have startProcessing() called on it.
        // `callback` must be invoked exactly once, either with a non-null instance and an
        // empty error, or with a null instance and a non-empty error message.
        virtual void createInstance(AudioPluginCatalogEntry* entry,
                                    const AudioPluginInstantiationOptions& options,
                                    std::function<void(std::unique_ptr<AudioPluginInstanceAPI> instance, std::string error)> callback) = 0;
    };

}
