#pragma once

#include "remidy.hpp"
#include "HostClasses.hpp"
#include "../GenericAudioBuses.hpp"

using namespace remidy_vst3;

namespace remidy {
    class PluginScannerVST3 : public FileBasedPluginScanning {
        void scanAllAvailablePluginsInPath(std::filesystem::path path, std::vector<PluginClassInfo>& infos);
        void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
        std::unique_ptr<PluginCatalogEntry> createPluginInformation(PluginClassInfo& info);

        PluginFormatVST3::Impl* impl{};

    public:
        explicit PluginScannerVST3(PluginFormatVST3::Impl* impl)
            : impl(impl) {
        }
        bool usePluginSearchPaths() override;
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;

        virtual bool isBlocklistedAsBundle(std::filesystem::path path) {
            // Vienna Synchron Player causes crash if (and only if) the code runs in debug mode.
            // It prevents our development, and they cause it intentionally.
            // It is not acceptable behavior as a plugin developer, so we do not approve their civil rights here.
            //
            // You can override this function to unblock it, but do it in your responsibility.
            if (path.string().contains("Vienna Synchron Player"))
                return true;
            return false;
        }
    };

    class PluginFormatVST3::Impl {
        PluginFormatVST3* owner;
        Logger* logger;
        Extensibility extensibility;
        PluginScannerVST3 scanning_;

        StatusCode doLoad(std::filesystem::path &vst3Dir, void** module) const;
        static StatusCode doUnload(std::filesystem::path &vst3Dir, void* module);
        std::function<StatusCode(std::filesystem::path &vst3Dir, void** module)> loadFunc;
        std::function<StatusCode(std::filesystem::path &vst3Dir, void* module)> unloadFunc;

        PluginBundlePool library_pool;
        HostApplication host;

    public:
        explicit Impl(PluginFormatVST3* owner) :
            owner(owner),
            // FIXME: should be provided by some means
            logger(Logger::global()),
            extensibility(*owner),
            scanning_(this),
            loadFunc([&](std::filesystem::path &vst3Dir, void** module)->StatusCode { return doLoad(vst3Dir, module); }),
            unloadFunc([&](std::filesystem::path &vst3Dir, void* module)->StatusCode { return doUnload(vst3Dir, module); }),
            library_pool(loadFunc,unloadFunc),
            host(logger) {
        }

        auto format() const { return owner; }
        Logger* getLogger() { return logger; }
        HostApplication* getHost() { return &host; }
        PluginBundlePool* libraryPool() { return &library_pool; }

        PluginExtensibility<PluginFormat>* getExtensibility();
        PluginScanning* scanning() { return &scanning_; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins();
        void forEachPlugin(std::filesystem::path& vst3Path,
            const std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)>& func,
            const std::function<void(void* module)>& cleanup
        );
        void unrefLibrary(PluginCatalogEntry* info);

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback);
    };

    class PluginInstanceVST3 : public PluginInstance {
        class ParameterSupport : public PluginParameterSupport {
            PluginInstanceVST3* owner;
            std::vector<PluginParameter*> parameter_defs{};
            // map<PerNoteControllerContext, definition>
            std::vector<std::pair<PerNoteControllerContext, std::vector<PluginParameter*>>> per_note_controller_defs{};
            v3_param_id *parameter_ids{};

        public:
            explicit ParameterSupport(PluginInstanceVST3* owner);
            ~ParameterSupport();

            bool accessRequiresMainThread() override { return true; }
            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext context) override;
            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override;
        };

        class VST3UmpInputDispatcher : public TypedUmpInputDispatcher {
            PluginInstanceVST3* owner;
            int32_t note_serial{1};

        public:
            explicit VST3UmpInputDispatcher(PluginInstanceVST3* owner) : owner(owner) {}

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
            PluginInstanceVST3* owner;

            std::vector<v3_audio_bus_buffers> inputAudioBusBuffersList{};
            std::vector<v3_audio_bus_buffers> outputAudioBusBuffersList{};

            std::vector<v3_speaker_arrangement> getVst3SpeakerConfigs(int32_t direction);

        public:
            explicit AudioBuses(PluginInstanceVST3* owner) : owner(owner) {
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

        class PluginStatesVST3 : public PluginStateSupport {
            PluginInstanceVST3* owner;

        public:
            explicit PluginStatesVST3(PluginInstanceVST3* owner) : owner(owner) {}

            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override;
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override;
        };

        class PresetsSupport : public PluginPresetsSupport {
            PluginInstanceVST3 *owner;
            std::vector<std::vector<PresetInfo>> banks{};

        public:
            PresetsSupport(PluginInstanceVST3* owner);
            bool isIndexStable() override { return true; }
            bool isIndexId() override { return true; }
            int32_t getPresetIndexForId(std::string &id) override;
            int32_t getPresetCount() override;
            PresetInfo getPresetInfo(int32_t index) override;
            void loadPreset(int32_t index) override;
        };

        PluginFormatVST3::Impl* owner;
        void* module;
        IPluginFactory* factory;
        std::string pluginName;
        IComponent* component;
        IAudioProcessor* processor;
        IEditController* controller;
        INoteExpressionController* note_expression_controller{nullptr};
        IUnitInfo* unit_info{nullptr};
        IMidiMapping* midi_mapping{nullptr};
        bool isControllerDistinctFromComponent;
        FUnknown* instance;
        // the connection point for IComponent, retrieved from the plugin.
        IConnectionPoint* connPointComp{nullptr};
        // the connection point for IEditController, retrieved from the plugin.
        IConnectionPoint* connPointEdit{nullptr};

        v3_process_data processData{};
        v3_process_context process_context{};
        HostEventList processDataInputEvents{};
        HostEventList processDataOutputEvents{};
        HostParameterChanges processDataInputParameterChanges{};
        HostParameterChanges processDataOutputParameterChanges{};

        void allocateProcessData(v3_process_setup& setup);

        AudioBuses* audio_buses{};

        ParameterSupport* _parameters{};
        PluginStatesVST3* _states{};
        PluginPresetsSupport* _presets{};

        VST3UmpInputDispatcher ump_input_dispatcher{this};

    public:
        explicit PluginInstanceVST3(
            PluginFormatVST3::Impl* owner,
            PluginCatalogEntry* info,
            void* module,
            IPluginFactory* factory,
            IComponent* component,
            IAudioProcessor* processor,
            IEditController* controller,
            bool isControllerDistinctFromComponent,
            FUnknown* instance
            );
        ~PluginInstanceVST3() override;

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

        // presets
        PluginPresetsSupport* presets() override;
    };

}
