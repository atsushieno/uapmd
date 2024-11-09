#pragma once

#include <atomic>

#include "remidy.hpp"
#include <lilv/lilv.h>

#include "lv2/LV2Helper.hpp"

namespace remidy {
    class AudioPluginScannerLV2 : public FileBasedPluginScanner {
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
        AudioPluginScannerLV2 lv2_scanner{nullptr};

    public:
        explicit Impl(PluginFormatLV2 *owner);

        ~Impl();

        auto format() const { return owner; }

        LilvWorld *world;
        remidy_lv2::LV2ImplWorldContext *worldContext;
        std::vector<LV2_Feature *> features{};

        PluginExtensibility<PluginFormat> *getExtensibility();

        PluginScanner *scanner() { return &lv2_scanner; }

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

    class AudioPluginInstanceLV2 : public PluginInstance {
        class ParameterSupport : public PluginParameterSupport {
            AudioPluginInstanceLV2 *owner;
            std::vector<PluginParameter *> parameter_defs{};
            std::vector<LV2ParameterHandler *> parameter_handlers{};

            void inspectParameters();

        public:
            explicit ParameterSupport(AudioPluginInstanceLV2 *owner) : owner(owner) {
                inspectParameters();
            }

            ~ParameterSupport();

            bool accessRequiresMainThread() override { return false; }

            std::vector<PluginParameter *> parameters() override;

            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;

            StatusCode getParameter(uint32_t index, double *value) override;
        };

        PluginFormatLV2::Impl *formatImpl;
        const LilvPlugin *plugin;
        LilvInstance *instance{nullptr};
        remidy_lv2::LV2ImplPluginContext implContext;

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
        std::vector<void *> port_buffers{};

        struct RemidyToLV2PortMapping {
            size_t bus;
            uint32_t channel;
            int32_t lv2Port;
        };
        std::vector<RemidyToLV2PortMapping> audio_in_port_mapping{};
        std::vector<RemidyToLV2PortMapping> audio_out_port_mapping{};

        ParameterSupport *_parameters{};

    public:
        explicit AudioPluginInstanceLV2(PluginCatalogEntry *entry, PluginFormatLV2::Impl *formatImpl,
                                        const LilvPlugin *plugin);

        ~AudioPluginInstanceLV2() override;

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
        bool hasEventInputs() override { return buses.numEventIn > 0; }

        bool hasEventOutputs() override { return buses.numEventOut > 0; }

        const std::vector<AudioBusConfiguration *> audioInputBuses() const override;

        const std::vector<AudioBusConfiguration *> audioOutputBuses() const override;

        // parameters
        PluginParameterSupport *parameters() override;
    };
}
