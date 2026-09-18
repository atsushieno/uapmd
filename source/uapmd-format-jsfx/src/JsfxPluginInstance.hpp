#pragma once

#include <memory>
#include <string>
#include <vector>

#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"
#include "ysfx.h"

namespace uapmd_jsfx {

    // The audio buses a JSFX effect has.
    //
    // They come from the script's `in_pin:` and `out_pin:` declarations and cannot be
    // changed, so there is one main input bus and one main output bus and nothing to
    // configure. A host that asked for a different channel count gets the declared one.
    class JsfxAudioBuses : public remidy::PluginAudioBuses {
        std::vector<remidy::AudioBusDefinition> definitions_{};
        std::vector<std::unique_ptr<remidy::AudioBusConfiguration>> owned_{};
        std::vector<remidy::AudioBusConfiguration*> inputs_{};
        std::vector<remidy::AudioBusConfiguration*> outputs_{};

    public:
        JsfxAudioBuses(uint32_t inputChannels, uint32_t outputChannels);

        // JSFX effects always have a MIDI input and output available to them: any script
        // may call midirecv() or midisend() without declaring anything.
        bool hasEventInputs() override { return true; }
        bool hasEventOutputs() override { return true; }

        const std::vector<remidy::AudioBusConfiguration*>& audioInputBuses() const override { return inputs_; }
        const std::vector<remidy::AudioBusConfiguration*>& audioOutputBuses() const override { return outputs_; }
    };

    // The sliders of a JSFX effect, presented as plugin parameters.
    //
    // JSFX describes its sliders more completely than most formats do: the declaration
    // carries a real minimum, maximum and increment, and enumerated sliders name their
    // values. That maps onto remidy's plain-valued parameters directly, with no
    // normalisation guesswork.
    class JsfxParameterSupport : public remidy::PluginParameterSupport {
        ysfx_t* fx_;
        std::vector<std::unique_ptr<remidy::PluginParameter>> owned_{};
        std::vector<remidy::PluginParameter*> parameters_{};
        std::vector<remidy::PluginParameter*> empty_{};
        std::vector<uint32_t> slider_indices_{};

    public:
        explicit JsfxParameterSupport(ysfx_t* fx);

        // Rebuilds the parameter list from the script. Sliders only exist once the effect
        // is loaded, and a recompiled script may have different ones.
        void rebuild();

        std::vector<remidy::PluginParameter*>& parameters() override { return parameters_; }

        // JSFX has no per-note controllers. Its sliders are the whole parameter story.
        std::vector<remidy::PluginParameter*>& perNoteControllers(
                remidy::PerNoteControllerContextTypes types,
                remidy::PerNoteControllerContext context) override {
            (void) types; (void) context;
            return empty_;
        }

        remidy::StatusCode setParameter(uint32_t index, double plainValue) override;
        remidy::StatusCode enqueueParameterRT(uint32_t index, double plainValue, uint64_t timestamp) override;
        remidy::StatusCode getParameter(uint32_t index, double* plainValue) override;

        remidy::StatusCode setPerNoteController(remidy::PerNoteControllerContext context, uint32_t index,
                                                double value) override;
        remidy::StatusCode enqueuePerNoteControllerRT(remidy::PerNoteControllerContext context, uint32_t index,
                                                      double value, uint64_t timestamp) override;
        remidy::StatusCode getPerNoteController(remidy::PerNoteControllerContext context, uint32_t index,
                                                double* value) override;

        std::string valueToString(uint32_t index, double value) override;
        std::string valueToStringPerNote(remidy::PerNoteControllerContext context, uint32_t index,
                                         double value) override;

        void refreshParameterMetadata(uint32_t index) override;

        // The slider number a parameter index refers to. Sliders are sparse in a script,
        // so the two are not the same thing.
        bool sliderForIndex(uint32_t index, uint32_t& slider) const;
    };

    // One loaded JSFX effect.
    class JsfxPluginInstance : public uapmd_plugin_hosting::AudioPluginInstanceBase {
        struct Impl;
        std::unique_ptr<Impl> impl_;

        JsfxPluginInstance(std::unique_ptr<Impl> impl, const std::string& displayName,
                           const std::string& pluginId);

        // Drains whatever the script sent with midisend() into the host's event output,
        // converting each MIDI 1.0 message into its single-word UMP form.
        void collectMidiOutput(remidy::AudioProcessContext& process);

    public:
        ~JsfxPluginInstance() override;

        // Loads and compiles the effect at `path`. Returns nothing and fills `error` when
        // the script does not load, which for JSFX means a syntax error rather than a
        // missing binary.
        static std::unique_ptr<JsfxPluginInstance> create(
                const std::filesystem::path& path,
                const std::string& pluginId,
                const uapmd_plugin_hosting::AudioPluginInstantiationOptions& options,
                std::string& error);

        uapmd_status_t startProcessing() override;
        uapmd_status_t stopProcessing() override;
        uapmd_status_t processAudio(remidy::AudioProcessContext& process) override;

        uint32_t latencyInSamples() const override;

        std::vector<uapmd_plugin_hosting::ParameterMetadata> parameterMetadataList() override;

        std::vector<uint8_t> saveStateSync() override;
        void loadStateSync(std::vector<uint8_t>& state) override;

        double getParameterValue(int32_t index) override;
        void setParameterValue(int32_t index, double value) override;
        void enqueueParameterValueRT(int32_t index, double value, uapmd_timestamp_t timestamp) override;
        std::string getParameterValueString(int32_t index, double value) override;

        remidy::PluginParameterSupport* parameterSupport() override;
        remidy::PluginAudioBuses* audioBuses() override;

        // There is an editor when the script has a `@gfx` section, but it is a pixel
        // buffer rather than a native view, so it is offered through
        // PluginFramebufferUIExtension. The native-view members below exist because the
        // interface has them: they track whether the editor is being displayed, which is
        // what starts and stops its drawing, and they succeed without creating anything.
        bool hasUISupport() override;
        bool createUI(bool isFloating, void* parentHandle,
                      std::function<bool(uint32_t, uint32_t)> resizeHandler) override;
        void destroyUI() override;
        bool showUI() override;
        void hideUI() override;
        bool isUIVisible() const override;
        bool canUIResize() override;
        bool getUISize(uint32_t& width, uint32_t& height) override;
        bool setUISize(uint32_t width, uint32_t height) override;

        uapmd_plugin_hosting::AudioPluginInstanceExtension* extension(std::string_view extensionId) override;
    };

}
