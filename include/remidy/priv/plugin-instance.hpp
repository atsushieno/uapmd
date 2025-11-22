#pragma once

#include "../remidy.hpp"
#include <optional>

namespace remidy {

    // [flags]
    enum PluginUIThreadRequirement : uint32_t {
        // AudioUnit and LV2, by default (probably bad behaved plugins can be explicitly marked as dirty = AllNonAudioOperation)
        None = 0,
        InstanceControl = 1,
        Parameters = 2,
        State = 4,
        // CLAP and VST3, by default (probably good plugins can be excluded out to switch to None)
        // Strictly speaking, CLAP does not require [main-thread] to everything, but it's close enough to label as everything.
        AllNonAudioOperation = 0xFFFFFFFF
    };

    class PluginInstance {
        PluginCatalogEntry* entry;

    protected:
        explicit PluginInstance(PluginCatalogEntry* entry) : entry(entry) {}

    public:
        struct ConfigurationRequest {
            uint32_t sampleRate{44100};
            uint32_t bufferSizeInSamples{4096};
            bool offlineMode{false};
            AudioContentType dataType{AudioContentType::Float32};
            std::optional<uint32_t> mainInputChannels{};
            std::optional<uint32_t> mainOutputChannels{};
            // In the future we should be able to configure audio buses.
            //std::optional<std::vector<BusConfiguration>> inputBuses{};
            //std::optional<std::vector<BusConfiguration>> outputBuses{};
        };

        virtual ~PluginInstance() = default;

        PluginCatalogEntry* info() { return entry; }

        virtual PluginExtensibility<PluginInstance>* getExtensibility() { return nullptr; }

        virtual PluginUIThreadRequirement requiresUIThreadOn() = 0;

        virtual StatusCode configure(ConfigurationRequest& configuration) = 0;

        virtual StatusCode startProcessing() = 0;

        virtual StatusCode stopProcessing() = 0;

        virtual StatusCode process(AudioProcessContext& process) = 0;

        virtual void setOfflineMode(bool offlineMode) { (void) offlineMode; }

        virtual PluginAudioBuses* audioBuses() = 0;

        virtual PluginParameterSupport* parameters() = 0;

        virtual PluginStateSupport* states() = 0;

        virtual PluginPresetsSupport* presets() = 0;

        virtual PluginUISupport* ui() = 0;
    };

}
