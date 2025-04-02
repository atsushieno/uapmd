#pragma once

#include <atomic>

#include "remidy.hpp"
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

        virtual StatusCode setParameter(double value, uint64_t timestamp) = 0;

        virtual StatusCode getParameter(double *value) {
            *value = current;
            return StatusCode::OK;
        }
    };

    class LV2AtomParameterHandler : public LV2ParameterHandler {
    public:
        LV2AtomParameterHandler(remidy_lv2::LV2ImplPluginContext &context, PluginParameter *def)
                : LV2ParameterHandler(context, def) {
        }

        ~LV2AtomParameterHandler() override = default;

        StatusCode setParameter(double value, uint64_t timestamp) override {
            current = value;
            // FIXME: add LV2 Atom for patch:Set
            std::cerr << "Not implemented yet" << std::endl;
            return StatusCode::OK;
        }
    };

    class LV2ControlPortParameterProxyPort : public LV2ParameterHandler {
        LV2_URID port_index;

    public:
        LV2ControlPortParameterProxyPort(uint32_t portIndex, remidy_lv2::LV2ImplPluginContext &context,
                                         PluginParameter *def)
                : LV2ParameterHandler(context, def), port_index(portIndex) {
        }

        ~LV2ControlPortParameterProxyPort() override = default;

        StatusCode setParameter(double value, uint64_t timestamp) override {
            current = value;
            // FIXME: set value on the corresponding ControlPort
            std::cerr << "Not implemented yet" << std::endl;
            return StatusCode::OK;
        }
    };

    class PluginInstanceLV2 : public PluginInstance {
        class ParameterSupport : public PluginParameterSupport {
            PluginInstanceLV2 *owner;
            std::vector<PluginParameter *> parameter_defs{};
            std::vector<LV2ParameterHandler *> parameter_handlers{};

            void inspectParameters();

        public:
            explicit ParameterSupport(PluginInstanceLV2 *owner) : owner(owner) {
                inspectParameters();
            }

            ~ParameterSupport();

            bool accessRequiresMainThread() override { return false; }

            std::vector<PluginParameter *> parameters() override;

            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;

            StatusCode getParameter(uint32_t index, double *value) override;
        };

        class LV2UmpInputDispatcher : public TypedUmpInputDispatcher {
            PluginInstanceLV2* owner;
            uint8_t midi1Bytes[16];

        public:
            LV2UmpInputDispatcher(PluginInstanceLV2* owner) : owner(owner) {}

            void enqueueMidi1Event(uint8_t atomInIndex, size_t eventSize);

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

        class LV2AudioBuses : public AudioBuses {
            PluginInstanceLV2* owner;

            struct BusSearchResult {
                uint32_t numEventIn{0};
                uint32_t numEventOut{0};
            };
            BusSearchResult buses;

            BusSearchResult inspectBuses();

            std::vector<AudioBusDefinition> input_bus_defs;
            std::vector<AudioBusDefinition> output_bus_defs;
            std::vector<AudioBusConfiguration *> input_buses;
            std::vector<AudioBusConfiguration *> output_buses;

        public:
            explicit LV2AudioBuses(PluginInstanceLV2* owner) : owner(owner) {
                buses = inspectBuses();
            }
            bool hasEventInputs() override { return buses.numEventIn > 0; }
            bool hasEventOutputs() override { return buses.numEventOut > 0; }

            const std::vector<AudioBusConfiguration*>& audioInputBuses() const override { return input_buses; }
            const std::vector<AudioBusConfiguration*>& audioOutputBuses() const override { return output_buses; }
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

        LV2AudioBuses* audio_buses{};

        ParameterSupport *_parameters{};

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
        AudioBuses* audioBuses() override { return audio_buses; }

        // parameters
        PluginParameterSupport *parameters() override;
    };
}
