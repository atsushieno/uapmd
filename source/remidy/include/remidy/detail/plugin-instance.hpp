#pragma once

#include "remidy/remidy.hpp"
#include "plugin-parameter.hpp"
#include <atomic>
#include <cstddef>
#include <optional>
#include <string_view>

namespace remidy {
    // A backend may report that plugin state changed, but the backend never owns
    // document dirty state. Hosts decide how to record this notification.
    using PluginStateChangeEvent = ParameterEventBase<void>;
    struct PluginTimingInfoChange {
        bool latency_changed{false};
        bool tail_changed{false};
    };
    using PluginTimingInfoChangeEvent = ParameterEventBase<void, PluginTimingInfoChange>;

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
        PluginStateChangeEvent plugin_state_change_event_{};
        PluginTimingInfoChangeEvent timing_info_change_event_{};

    protected:
        explicit PluginInstance(PluginCatalogEntry* entry) : entry(entry) {}

        void notifyPluginStateChanged() {
            plugin_state_change_event_.notify();
        }

        void notifyTimingInfoChanged(PluginTimingInfoChange change) {
            if (!change.latency_changed && !change.tail_changed)
                return;
            timing_info_change_event_.notify(change);
        }

    public:
        struct ConfigurationRequest {
            uint32_t sampleRate{48000};
            uint32_t bufferSizeInSamples{4096};
            bool offlineMode{false};
            AudioContentType dataType{AudioContentType::Float32};
            std::optional<uint32_t> mainInputChannels{};
            std::optional<uint32_t> mainOutputChannels{};
        };

        virtual ~PluginInstance() = default;

        PluginCatalogEntry* info() { return entry; }

        PluginStateChangeEvent& pluginStateChangeEvent() { return plugin_state_change_event_; }
        PluginTimingInfoChangeEvent& timingInfoChangeEvent() { return timing_info_change_event_; }

        virtual PluginExtensibility<PluginInstance>* getExtensibility(std::string_view extensionId) {
            (void) extensionId;
            return nullptr;
        }

        virtual PluginUIThreadRequirement requiresUIThreadOn() = 0;

        virtual StatusCode configure(ConfigurationRequest& configuration) = 0;

        virtual StatusCode startProcessing() = 0;

        virtual StatusCode stopProcessing() = 0;

        virtual StatusCode process(AudioProcessContext& process) = 0;

        virtual uint32_t latencyInSamples() const = 0;
        virtual double tailLengthInSeconds() const = 0;

        virtual PluginAudioBuses* audioBuses() = 0;

        virtual PluginParameterSupport* parameters() = 0;

        virtual PluginStateSupport* states() = 0;

        virtual PluginPresetsSupport* presets() = 0;

        virtual PluginUISupport* ui() = 0;

        // Some plugin APIs (e.g. CLAP in-place pairs) require the host to provide replacing buffers.
        // Hosts can query this flag to prepare AudioProcessContext inputs accordingly.
        virtual bool requiresReplacingProcess() const = 0;
    };

}
