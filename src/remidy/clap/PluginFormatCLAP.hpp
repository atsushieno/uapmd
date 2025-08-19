#pragma once

#include "remidy.hpp"
#include "HostClasses.hpp"
#include "../GenericAudioBuses.hpp"
#include "clap/factory/plugin-factory.h"
#include "clap/plugin.h"
#include "clap/helpers/event-list.hh"

namespace remidy {
    class PluginScannerCLAP : public FileBasedPluginScanning {
        void scanAllAvailablePluginsInPath(std::filesystem::path path, std::vector<std::unique_ptr<PluginCatalogEntry>>& entries);
        void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<std::unique_ptr<PluginCatalogEntry>>& entries);

        PluginFormatCLAP::Impl* impl{};

    public:
        explicit PluginScannerCLAP(PluginFormatCLAP::Impl* impl)
            : impl(impl) {
        }
        bool usePluginSearchPaths() override;
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;

        virtual bool isBlocklistedAsBundle(std::filesystem::path path) {
            return false;
        }
    };

    class PluginFormatCLAP::Impl {
        Logger* logger;
        Extensibility extensibility;
        PluginScannerCLAP scanning_;

        StatusCode doLoad(std::filesystem::path &clapPath, void** module) const;
        static StatusCode doUnload(std::filesystem::path &clapPath, void* module);
        std::function<StatusCode(std::filesystem::path &clapPath, void** module)> loadFunc;
        std::function<StatusCode(std::filesystem::path &clapPath, void* module)> unloadFunc;

        PluginBundlePool library_pool;

        RemidyCLAPHost host;

    public:
        explicit Impl(PluginFormatCLAP* owner) :
            owner(owner),
            // FIXME: should be provided by some means
            logger(Logger::global()),
            extensibility(*owner),
            scanning_(this),
            loadFunc([&](std::filesystem::path &clapDir, void** module)->StatusCode { return doLoad(clapDir, module); }),
            unloadFunc([&](std::filesystem::path &clapDir, void* module)->StatusCode { return doUnload(clapDir, module); }),
            library_pool(loadFunc,unloadFunc) {
        }

        PluginFormatCLAP* owner;

        auto format() const { return owner; }
        Logger* getLogger() { return logger; }
        PluginBundlePool* libraryPool() { return &library_pool; }

        PluginExtensibility<PluginFormat>* getExtensibility();
        PluginScanning* scanning() { return &scanning_; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins();
        void forEachPlugin(std::filesystem::path& clapDir,
            const std::function<void(void* module, clap_plugin_factory_t* factory, const clap_plugin_descriptor_t* info)>& func,
            const std::function<void(void* module)>& cleanup
        );
        void unrefLibrary(PluginCatalogEntry* info);

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback);
    };

    class PluginInstanceCLAP : public PluginInstance {
        PluginFormatCLAP::Impl* owner;
        const clap_plugin_t* plugin;
        void* module;
        clap_process_t clap_process;

        std::vector<clap_audio_buffer_t> audio_in_port_buffers{};
        std::vector<clap_audio_buffer_t> audio_out_port_buffers{};
        std::vector<clap_event_transport_t> transports_events{};

        class ParameterSupport : public PluginParameterSupport {
            PluginInstanceCLAP* owner;
            std::vector<PluginParameter*> parameter_defs{};
            std::vector<clap_id> parameter_ids{};
            std::vector<void*> parameter_cookies{};
            clap_plugin_params_t* params_ext;

        public:
            explicit ParameterSupport(PluginInstanceCLAP* owner);
            ~ParameterSupport();

            bool accessRequiresMainThread() override { return true; }
            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext context) override;
            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override;
        };

        class CLAPUmpInputDispatcher : public TypedUmpInputDispatcher {
            PluginInstanceCLAP* owner;
            int32_t note_serial{1};

        public:
            explicit CLAPUmpInputDispatcher(PluginInstanceCLAP* owner) : owner(owner) {}

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
        };

        class AudioBuses : public GenericAudioBuses {
            PluginInstanceCLAP* owner;

        public:
            explicit AudioBuses(PluginInstanceCLAP* owner) : owner(owner) {
                inspectBuses();
            }
            ~AudioBuses() override {
                for (const auto bus: audio_in_buses)
                    delete bus;
                for (const auto bus: audio_out_buses)
                    delete bus;
            }

            void configure(ConfigurationRequest& config);
            void allocateBuffers();
            void deallocateBuffers();
            void deactivateAllBuses();

            void inspectBuses() override;
        };

        class PluginStatesCLAP : public PluginStateSupport {
            PluginInstanceCLAP* owner;
            clap_plugin_state_context_t* state_context_ext;
            clap_plugin_state_t* state_ext;

        public:
            explicit PluginStatesCLAP(PluginInstanceCLAP* owner);

            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override;
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override;
        };

        class PresetsSupport : public PluginPresetsSupport {
            PluginInstanceCLAP *owner;
            std::vector<PresetInfo> items{};

        public:
            PresetsSupport(PluginInstanceCLAP* owner);
            bool isIndexStable() override { return false; }
            bool isIndexId() override { return false; }
            int32_t getPresetIndexForId(std::string &id) override;
            int32_t getPresetCount() override;
            PresetInfo getPresetInfo(int32_t index) override;
            void loadPreset(int32_t index) override;
        };

        clap::helpers::EventList events{};
        AudioBuses* audio_buses{};
        ParameterSupport* _parameters{};
        PluginStateSupport* _states{};
        CLAPUmpInputDispatcher ump_input_dispatcher{this};

        void remidyProcessContextToClapProcess(clap_process_t& dst, AudioProcessContext& src);
        void clapProcessToRemidyProcessContext(AudioProcessContext& dst, clap_process_t& src);
        void resizeAudioPortBuffers(size_t newSize, bool isDouble);
        void resetAudioPortBuffers();
        void cleanupBuffers();

    public:
        explicit PluginInstanceCLAP(
            PluginFormatCLAP::Impl* owner,
            PluginCatalogEntry* info,
            void* module,
            const clap_plugin_t* plugin
            );
        ~PluginInstanceCLAP() override;

        PluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return owner->format()->requiresUIThreadOn(info());
        }

        // audio processing core features
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;
        StatusCode process(AudioProcessContext &process) override;

        // port helpers
        PluginAudioBuses* audioBuses() override { return audio_buses; }

        // parameters
        PluginParameterSupport* parameters() override;

        // states
        PluginStateSupport* states() override;
    };
}

