#pragma once

#include "remidy.hpp"
#include <aap/core/host/plugin-connections.h>
#include <aap/core/host/plugin-host.h>

namespace remidy {
    class PluginFormatAAPImpl;
    class PluginInstanceAAP;

    class PluginScanningAAP : public PluginScanning {
        PluginFormatAAPImpl* owner;

    public:
        PluginScanningAAP(PluginFormatAAPImpl* owner) : owner(owner) {}
        ~PluginScanningAAP() override = default;

        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }

        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }

        std::vector<std::unique_ptr<PluginCatalogEntry>>
        scanAllAvailablePlugins(bool requireFastScanning) override;
    };

    class PluginInstanceAAP : public PluginInstance {

        class PluginBusesAAP : public PluginAudioBuses {
            remidy::PluginInstanceAAP* owner;
            // FIXME: AAP doesn't have decent bus configuration API yet.
            AudioBusDefinition main_bus_def_in{"main", AudioBusRole::Main, {AudioChannelLayout::mono()}};
            AudioBusDefinition main_bus_def_out{"main", AudioBusRole::Main, {AudioChannelLayout::stereo()}};
            AudioBusConfiguration main_bus_in{main_bus_def_in};
            AudioBusConfiguration main_bus_out{main_bus_def_out};
            std::vector<AudioBusConfiguration*> main_buses_in{&main_bus_in};
            std::vector<AudioBusConfiguration*> main_buses_out{&main_bus_out};

        public:
            PluginBusesAAP(remidy::PluginInstanceAAP* owner) : owner(owner) {}

            bool hasEventInputs() override;
            bool hasEventOutputs() override;

            const std::vector<AudioBusConfiguration *> &audioInputBuses() const override {
                return main_buses_in;
            }

            const std::vector<AudioBusConfiguration *> &audioOutputBuses() const override {
                return main_buses_out;
            }
        };

        class ParameterSupport : public PluginParameterSupport {
            PluginInstanceAAP* owner;
            std::vector<remidy::PluginParameter *> parameter_list{};
            std::vector<remidy::PluginParameter *> per_note_controller_list{};

        public:
            ParameterSupport(PluginInstanceAAP* owner);

            std::vector<PluginParameter *> &parameters() override {
                return parameter_list;
            }

            std::vector<PluginParameter *> &perNoteControllers(PerNoteControllerContextTypes types,
                                                               PerNoteControllerContext context) override {
                return per_note_controller_list;
            }

            StatusCode setParameter(uint32_t index, double plainValue, uint64_t timestamp) override;

            StatusCode getParameter(uint32_t index, double *plainValue) override;

            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index,
                                            double value, uint64_t timestamp) override;

            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index,
                                            double *value) override;

            std::string valueToString(uint32_t index, double value) override;

            std::string valueToStringPerNote(PerNoteControllerContext context, uint32_t index,
                                             double value) override;

        };

        class PresetsSupport : public PluginPresetsSupport {
            remidy::PluginInstanceAAP* owner;

        public:
            PresetsSupport(remidy::PluginInstanceAAP* owner) : owner(owner) {}

            bool isIndexStable() override {
                // FIXME: implement
                return false;
            }

            bool isIndexId() override {
                // FIXME: implement
                return false;
            }

            int32_t getPresetIndexForId(std::string &id) override {
                // FIXME: implement
                return 0;
            }

            int32_t getPresetCount() override {
                // FIXME: implement
                return 0;
            }

            PresetInfo getPresetInfo(int32_t index) override {
                // FIXME: implement
                return PresetInfo{"", "", 0, 0};
            }

            void loadPreset(int32_t index) override {
                // FIXME: implement
            }
        };

        class UISupport : public PluginUISupport {
            remidy::PluginInstanceAAP* owner;

        public:
            UISupport(remidy::PluginInstanceAAP* owner) : owner(owner) {}

            bool hasUI() override {
                // FIXME: implement
                return true;
            }

            bool create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
                // FIXME: implement
                return false;
            }
            void destroy() override {
                // FIXME: implement
            }

            bool show() override {
                // FIXME: implement
                return false;
            }
            void hide() override {
                // FIXME: implement
            }

            void setWindowTitle(std::string title) override {
                // FIXME: implement
            }

            bool canResize()  override {
                // FIXME: implement
                return false;
            }

            bool getSize(uint32_t &width, uint32_t &height)  override {
                // FIXME: implement
                return false;
            }
            bool setSize(uint32_t width, uint32_t height)  override {
                // FIXME: implement
                return false;
            }
            bool suggestSize(uint32_t &width, uint32_t &height)  override {
                // FIXME: implement
                return false;
            }

            bool setScale(double scale)  override {
                // FIXME: implement
                return false;
            }
        };

        class StateSupport : public PluginStateSupport {
            remidy::PluginInstanceAAP* owner;

        public:
            StateSupport(remidy::PluginInstanceAAP* owner) : owner(owner) {}

            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override {
                // FIXME: implement
                return {};
            }
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override {
                // FIXME: implement
            }
        };

        PluginFormatAAPImpl* format;
        PluginBusesAAP buses{this};
        ParameterSupport params{this};
        PresetsSupport presets_{this};
        StateSupport state_{this};
        UISupport ui_{this};
        aap::PluginInstance* instance;

    public:
        PluginInstanceAAP(PluginFormatAAPImpl* format, PluginCatalogEntry* entry, aap::PluginInstance* aapInstance);

        aap::PluginInstance* aapInstance() { return instance; };

        PluginUIThreadRequirement requiresUIThreadOn() override { return PluginUIThreadRequirement::None; }

        StatusCode configure(ConfigurationRequest &configuration) override;

        StatusCode startProcessing() override;

        StatusCode stopProcessing() override;

        StatusCode process(AudioProcessContext &process) override;

        PluginAudioBuses *audioBuses() override { return &buses; }

        PluginParameterSupport *parameters() override { return &params; }

        PluginStateSupport *states() override { return &state_; }

        PluginPresetsSupport *presets() override { return &presets_; }

        PluginUISupport *ui() override { return &ui_; }

    };

    class PluginFormatAAPImpl : public PluginFormatAAP {
        aap::PluginListSnapshot plugin_list_snapshot;
        aap::PluginClientConnectionList* plugin_client_connections;
        std::unique_ptr<aap::PluginClient> android_host;
        PluginScanningAAP scanning_{this};

    public:
        PluginFormatAAPImpl();

        PluginScanning *scanning() override { return &scanning_; }

        void createInstance(PluginCatalogEntry *info, PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance>,
                                               std::string)> callback) override;

    };
}
