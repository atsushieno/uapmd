#pragma once

#include <atomic>

#include "remidy.hpp"
#include "../GenericAudioBuses.hpp"
#include "lilv/lilv.h"

#include "LV2Helper.hpp"

namespace remidy {
    class AudioPluginScannerLV2 : public FileBasedPluginScanning {
        LilvWorld *world;
    public:
        explicit AudioPluginScannerLV2(LilvWorld *world) : world(world) {}

        bool usePluginSearchPaths() override { return true; }

        std::vector<std::filesystem::path> &getDefaultSearchPaths() override;

        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }

        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }

        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatLV2::Impl {
        PluginFormatLV2 *owner;
        Logger *logger;
        Extensibility extensibility;
        AudioPluginScannerLV2 scanning_{nullptr};

    public:
        explicit Impl(PluginFormatLV2 *owner);

        ~Impl();

        auto getLogger() { return logger; }

        auto format() const { return owner; }

        LilvWorld *world;
        remidy_lv2::LV2ImplWorldContext *worldContext;
        std::vector<LV2_Feature *> features{};

        PluginExtensibility<PluginFormat> *getExtensibility();

        PluginScanning *scanning() { return &scanning_; }

        void createInstance(PluginCatalogEntry *info,
                            std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback);

        void unrefLibrary(PluginCatalogEntry &info);

        PluginCatalog createCatalogFragment(const std::filesystem::path &bundlePath);
    };

    class LV2ParameterHandler {
    protected:
        remidy_lv2::LV2ImplPluginContext &context;
        PluginParameter *def;
        double current;

    public:
        LV2ParameterHandler(remidy_lv2::LV2ImplPluginContext &context, PluginParameter *def)
                : context(context), def(def), current(def->defaultValue()) {

        }

        virtual ~LV2ParameterHandler() = default;

        virtual StatusCode setParameter(double value, remidy_timestamp_t timestamp) = 0;

        virtual StatusCode getParameter(double *value) {
            *value = current;
            return StatusCode::OK;
        }
    };

    class PluginInstanceLV2 : public PluginInstance {
        class ParameterSupport : public PluginParameterSupport {
            std::vector<PluginParameter *> parameter_defs{};
            std::vector<LV2ParameterHandler *> parameter_handlers{};

            void inspectParameters();

        public:
            explicit ParameterSupport(PluginInstanceLV2 *owner) : owner(owner) {
                inspectParameters();
            }

            ~ParameterSupport();

            PluginInstanceLV2 *owner;

            bool accessRequiresMainThread() override { return false; }

            std::vector<PluginParameter *>& parameters() override;
            std::vector<PluginParameter *>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext note) override {
                // not supported in LV2
                static std::vector<PluginParameter *> empty {};
                return empty;
            }

            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override {
                owner->formatImpl->getLogger()->logError("Per-note controller is not supported in LV2");
                return StatusCode::INVALID_PARAMETER_OPERATION;
            }
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override {
                owner->formatImpl->getLogger()->logError("Per-note controller is not supported in LV2");
                return StatusCode::INVALID_PARAMETER_OPERATION;
            }
        };

        class LV2AtomParameterHandler : public LV2ParameterHandler {
            ParameterSupport* owner;

        public:
            LV2AtomParameterHandler(ParameterSupport* owner, remidy_lv2::LV2ImplPluginContext &context, PluginParameter *def)
                    : LV2ParameterHandler(context, def), owner(owner) {
            }

            ~LV2AtomParameterHandler() override = default;

            StatusCode setParameter(double value, remidy_timestamp_t timestamp) override {
                current = value;
                owner->owner->ump_input_dispatcher.enqueuePatchSetEvent(def->index(), value, timestamp);
                return StatusCode::OK;
            }
        };

        class LV2ControlPortParameterProxyPort : public LV2ParameterHandler {
            ParameterSupport* owner;
            LV2_URID port_index;

        public:
            LV2ControlPortParameterProxyPort(ParameterSupport* owner, uint32_t portIndex, remidy_lv2::LV2ImplPluginContext &context,
                                             PluginParameter *def)
                    : LV2ParameterHandler(context, def), owner(owner), port_index(portIndex) {
            }

            ~LV2ControlPortParameterProxyPort() override = default;

            StatusCode setParameter(double value, remidy_timestamp_t timestamp) override {
                // timestamp cannot be supported for ControlPort.
                current = value;
                owner->owner->implContext.control_buffer_pointers[port_index] = static_cast<float>(value);
                return StatusCode::OK;
            }
        };

        class LV2UmpInputDispatcher : public TypedUmpInputDispatcher {
            PluginInstanceLV2* owner;
            uint8_t midi1Bytes[16];

        public:
            LV2UmpInputDispatcher(PluginInstanceLV2* owner) : owner(owner) {}

            void enqueueMidi1Event(uint8_t atomInIndex, size_t eventSize);
            void enqueuePatchSetEvent(int32_t index, double value, remidy_timestamp_t timestamp);
            int32_t atom_context_group; // maybe this should be a setter?

            void onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) override;
            void onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index, uint32_t data) override;
            void onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) override;
            void onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) override;
            void onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) override;
            void onPressure(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) override;
            void onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags, remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) override;
            void onRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) override;
            void onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) override;
            void onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) override;
            void onProcessStart(remidy::AudioProcessContext &src) override;
            void onProcessEnd(remidy::AudioProcessContext &src) override;
        };

        class AudioBuses : public GenericAudioBuses {
            PluginInstanceLV2* owner;

        public:
            explicit AudioBuses(PluginInstanceLV2* owner) : owner(owner) {
                inspectBuses();
            }
            void inspectBuses() override;
        };

        class PluginStatesLV2 : public PluginStateSupport {
            PluginInstanceLV2* owner;

        public:
            explicit PluginStatesLV2(PluginInstanceLV2* owner) : owner(owner) {}

            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override;
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override;
        };

        class PresetsSupport : public PluginPresetsSupport {
            PluginInstanceLV2 *owner;
            std::vector<PresetInfo> items{};
            LilvNodes *preset_nodes{};

        public:
            PresetsSupport(PluginInstanceLV2* owner);
            bool isIndexStable() override { return false; }
            bool isIndexId() override { return false; }
            int32_t getPresetIndexForId(std::string &id) override;
            int32_t getPresetCount() override;
            PresetInfo getPresetInfo(int32_t index) override;
            void loadPreset(int32_t index) override;
        };

        class UISupport : public PluginUISupport {
        public:
            explicit UISupport(PluginInstanceLV2* owner);
            ~UISupport() override = default;
            bool create() override;
            void destroy() override;
            bool show() override;
            void hide() override;
            void setWindowTitle(std::string title) override;
            bool attachToParent(void *parent) override;
            bool canResize() override;
            bool getSize(uint32_t &width, uint32_t &height) override;
            bool setSize(uint32_t width, uint32_t height) override;
            bool suggestSize(uint32_t &width, uint32_t &height) override;
            bool setScale(double scale) override;
        };

        PluginFormatLV2::Impl *formatImpl;
        int32_t sample_rate;
        const LilvPlugin *plugin;
        LilvInstance *instance{nullptr};
        remidy_lv2::LV2ImplPluginContext implContext;

        struct LV2PortInfo {
            void* port_buffer{nullptr};
            int32_t atom_in_index{-1};
            int32_t atom_out_index{-1};
            size_t buffer_size{0};
            LV2_Atom_Forge forge{};
            LV2_Atom_Forge_Frame frame{};
        };
        std::vector<LV2PortInfo> lv2_ports{};

        struct RemidyToLV2PortMapping {
            size_t bus;
            uint32_t channel;
            int32_t lv2Port;
        };
        std::vector<RemidyToLV2PortMapping> audio_in_port_mapping{};
        std::vector<RemidyToLV2PortMapping> audio_out_port_mapping{};

        AudioBuses* audio_buses{};

        ParameterSupport *_parameters{};
        PluginStateSupport *_states{};
        PluginPresetsSupport *_presets{};
        PluginUISupport *_ui{};

        LV2UmpInputDispatcher ump_input_dispatcher{this};

        int32_t portIndexForAtomGroupIndex(bool isInput, uint8_t atomGroup) {
            for (int i = 0, n = lv2_ports.size(); i < n; i++)
                if (lv2_ports[i].atom_in_index == atomGroup && !isInput ||
                    lv2_ports[i].atom_out_index == atomGroup && isInput)
                    return i;
            return -1;
        }
        LV2_URID_Map* getLV2UridMapData() {
            return &implContext.statics->features.urid_map_feature_data;
        }
        LV2_URID_Unmap* getLV2UridUnmapData() {
            return &implContext.statics->features.urid_unmap_feature_data;
        }

    public:
        explicit PluginInstanceLV2(PluginCatalogEntry *entry, PluginFormatLV2::Impl *formatImpl,
                                   const LilvPlugin *plugin);

        ~PluginInstanceLV2() override;

        PluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return formatImpl->format()->requiresUIThreadOn(info());
        }

        // audio processing core functions.
        StatusCode configure(ConfigurationRequest &configuration) override;

        StatusCode startProcessing() override;

        StatusCode stopProcessing() override;

        StatusCode process(AudioProcessContext &process) override;

        // port helpers
        PluginAudioBuses* audioBuses() override { return audio_buses; }

        // parameters
        PluginParameterSupport *parameters() override;

        // states
        PluginStateSupport *states() override;

        // presets
        PluginPresetsSupport *presets() override;

        // ui
        PluginUISupport *ui() override;
    };
}
