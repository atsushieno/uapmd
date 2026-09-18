#pragma once

#include <string>
#include "AudioPluginInstanceAPI.hpp"

namespace uapmd_plugin_hosting {

    // Default implementations for the parts of AudioPluginInstanceAPI that a plugin format
    // does not necessarily have.
    //
    // The interface is wide because it has to describe every format we ship, but a given
    // format answers only some of it meaningfully: a format with no editor, no presets and
    // no per-note controllers still has to supply those members. This base answers them the
    // way a format that lacks the feature should, so an implementation is left with the
    // parts that are actually about the plugin.
    //
    // What stays pure virtual here is the contract a format cannot avoid: identity,
    // processing, parameters, and the two remidy facades. Everything else can be overridden
    // when the format does support it.
    class AudioPluginInstanceBase : public AudioPluginInstanceAPI {
        mutable std::string display_name_{};
        mutable std::string format_name_{};
        mutable std::string plugin_id_{};
        bool bypassed_{false};
        remidy::PluginTimingInfoChangeEvent timing_info_change_event_{};

    protected:
        AudioPluginInstanceBase(std::string displayName, std::string formatName, std::string pluginId) :
                display_name_(std::move(displayName)),
                format_name_(std::move(formatName)),
                plugin_id_(std::move(pluginId)) {
        }

        // Reports that latency or tail length changed, so the host can recompensate.
        void notifyTimingInfoChanged(remidy::PluginTimingInfoChange change) {
            if (!change.latency_changed && !change.tail_changed)
                return;
            timing_info_change_event_.notify(change);
        }

    public:
        std::string& displayName() const override { return display_name_; }
        std::string& formatName() const override { return format_name_; }
        std::string& pluginId() const override { return plugin_id_; }

        // Host-side bypass. A format that can bypass inside the plugin overrides both.
        bool bypassed() const override { return bypassed_; }
        void bypassed(bool value) override { bypassed_ = value; }

        uint32_t latencyInSamples() const override { return 0; }
        double tailLengthInSeconds() const override { return 0.0; }
        // Every format we ship reports false; a format that cannot process in place says so.
        bool requiresReplacingProcess() const override { return false; }

        std::vector<ParameterMetadata> perNoteControllerMetadataList(
                remidy::PerNoteControllerContextTypes contextType, uint32_t context) override {
            (void) contextType;
            (void) context;
            return {};
        }

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override {
            (void) note; (void) index; (void) value;
        }
        void enqueuePerNoteControllerValueRT(uint8_t note, uint8_t index, double value,
                                             uapmd_timestamp_t timestamp) override {
            (void) note; (void) index; (void) value; (void) timestamp;
        }
        std::string getPerNoteControllerValueString(uint8_t note, uint8_t index, double value) override {
            (void) note; (void) index; (void) value;
            return {};
        }

        std::vector<PresetsMetadata> presetMetadataList() override { return {}; }
        void loadPreset(int32_t presetIndex) override { (void) presetIndex; }
        void loadPreset(int32_t presetIndex,
                        std::function<void(std::string error, void* callbackContext)> completed) override {
            (void) presetIndex;
            if (completed)
                completed("this plugin format does not support presets", nullptr);
        }

        // The defaults describe a format that carries no state. Note that the sequencer
        // still saves and loads projects through the synchronous pair, so a format with
        // state must implement those and not only the asynchronous ones.
        std::vector<uint8_t> saveStateSync() override { return {}; }
        void loadStateSync(std::vector<uint8_t>& state) override { (void) state; }

        void requestState(StateContextType stateContextType, bool includeUiState, void* callbackContext,
                          std::function<void(std::vector<uint8_t> state, std::string error,
                                             void* callbackContext)> receiver) override {
            (void) stateContextType;
            (void) includeUiState;
            if (receiver)
                receiver(saveStateSync(), "", callbackContext);
        }

        void loadState(std::vector<uint8_t> state, StateContextType stateContextType, bool includeUiState,
                       void* callbackContext,
                       std::function<void(std::string error, void* callbackContext)> completed) override {
            (void) stateContextType;
            (void) includeUiState;
            loadStateSync(state);
            if (completed)
                completed("", callbackContext);
        }

        bool hasUISupport() override { return false; }
        bool createUI(bool isFloating, void* parentHandle,
                      std::function<bool(uint32_t, uint32_t)> resizeHandler) override {
            (void) isFloating; (void) parentHandle; (void) resizeHandler;
            return false;
        }
        void destroyUI() override {}
        bool showUI() override { return false; }
        void hideUI() override {}
        bool isUIVisible() const override { return false; }
        bool setUISize(uint32_t width, uint32_t height) override {
            (void) width; (void) height;
            return false;
        }
        bool getUISize(uint32_t& width, uint32_t& height) override {
            (void) width; (void) height;
            return false;
        }
        bool canUIResize() override { return false; }

        remidy::EventListenerId addTimingInfoChangeListener(
                std::function<void(remidy::PluginTimingInfoChange)> listener) override {
            return timing_info_change_event_.addListener(std::move(listener));
        }
        void removeTimingInfoChangeListener(remidy::EventListenerId listenerId) override {
            timing_info_change_event_.removeListener(listenerId);
        }
    };

}
