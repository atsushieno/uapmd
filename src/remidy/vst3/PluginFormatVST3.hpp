#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include "remidy/remidy.hpp"
#include "HostClasses.hpp"
#include "../GenericAudioBuses.hpp"

using namespace remidy_vst3;

namespace remidy {
    class PluginFormatVST3Impl;

    class PluginScannerVST3 : public FileBasedPluginScanning {
        void scanAllAvailablePluginsInPath(std::filesystem::path path, std::vector<PluginClassInfo>& infos);
        void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
        std::unique_ptr<PluginCatalogEntry> createPluginInformation(PluginClassInfo& info);

        PluginFormatVST3Impl* owner{};

    public:
        explicit PluginScannerVST3(PluginFormatVST3Impl* owner)
            : owner(owner) {
        }
        bool usePluginSearchPaths() override;
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatVST3Impl : public PluginFormatVST3 {
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
        explicit PluginFormatVST3Impl(std::vector<std::string>& overrideSearchPaths) :
            logger(Logger::global()),
            extensibility(*this),
            scanning_(this),
            loadFunc([&](std::filesystem::path &vst3Dir, void** module)->StatusCode { return doLoad(vst3Dir, module); }),
            unloadFunc([&](std::filesystem::path &vst3Dir, void* module)->StatusCode { return doUnload(vst3Dir, module); }),
            library_pool(loadFunc,unloadFunc),
            host(logger) {
        }

        Logger* getLogger() { return logger; }
        HostApplication* getHost() { return &host; }
        PluginBundlePool* libraryPool() { return &library_pool; }

        PluginExtensibility<PluginFormat>* getExtensibility() override;
        PluginScanning* scanning() override { return &scanning_; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins();
        void forEachPlugin(std::filesystem::path& vst3Path,
            const std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)>& func,
            const std::function<void(void* module)>& cleanup
        );
        void unrefLibrary(PluginCatalogEntry* info);

        void createInstance(PluginCatalogEntry* info,
                            PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) override;
    };

    class PluginInstanceVST3 : public PluginInstance {
        class ParameterSupport : public PluginParameterSupport {
            PluginInstanceVST3* owner;
            std::vector<PluginParameter*> parameter_defs{};
            std::unordered_map<ParamID, uint32_t> param_id_to_index{};
            std::vector<std::unique_ptr<PluginParameter>> per_note_controller_storage{};
            std::vector<PluginParameter*> per_note_controller_defs{};
            struct NoteExpressionKey {
                uint32_t group;
                uint32_t channel;
                uint32_t index;
                bool operator==(const NoteExpressionKey& other) const {
                    return group == other.group && channel == other.channel && index == other.index;
                }
            };
            struct NoteExpressionKeyHash {
                size_t operator()(const NoteExpressionKey& key) const {
                    size_t h1 = std::hash<uint32_t>{}(key.group);
                    size_t h2 = std::hash<uint32_t>{}(key.channel);
                    size_t h3 = std::hash<uint32_t>{}(key.index);
                    return ((h1 ^ (h2 << 1)) >> 1) ^ (h3 << 1);
                }
            };
            std::unordered_map<NoteExpressionKey, NoteExpressionTypeInfo, NoteExpressionKeyHash> note_expression_info_map{};
            std::vector<ParamID> parameter_ids{};
            ParamID program_change_parameter_id{static_cast<ParamID>(-1)};
            int32_t program_change_parameter_index{-1};
            std::unordered_map<UnitID, std::pair<UnitID, std::string>> unit_hierarchy{};
            std::unordered_map<UnitID, std::string> unit_path_cache{};

            void clearParameterList();
            void populateParameterList();
            void rebuildParameterList();
            void broadcastAllParameterValues();
            void buildUnitHierarchy();
            std::string buildUnitPath(UnitID unitId);

        public:
            explicit ParameterSupport(PluginInstanceVST3* owner);
            ~ParameterSupport() override;

            void refreshAllParameterMetadata() override;

            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext context) override;
            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override;

            std::string valueToString(uint32_t index, double value) override;
            std::string valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) override;
            void refreshParameterMetadata(uint32_t index) override;
            std::optional<uint32_t> indexForParamId(ParamID id) const;
            void notifyParameterValue(ParamID id, double plainValue);
            ParamID getParameterId(uint32_t index) const { return index < parameter_ids.size() ? parameter_ids[index] : 0; }

            ParamID getProgramChangeParameterId() const { return program_change_parameter_id; }
            int32_t getProgramChangeParameterIndex() const { return program_change_parameter_index; }

            void setProgramChange(uint4_t group, uint4_t channel, uint7_t flags, uint7_t program, uint7_t bankMSB,
                                  uint7_t bankLSB);
        private:
            void clearNoteExpressionInfo(uint32_t group, uint32_t channel);
            const NoteExpressionTypeInfo* resolveNoteExpressionInfo(uint32_t group, uint32_t channel, uint32_t index);
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

            std::vector<AudioBusBuffers> inputAudioBusBuffersList{};
            std::vector<AudioBusBuffers> outputAudioBusBuffersList{};

            std::vector<SpeakerArrangement> getVst3SpeakerConfigs(int32_t direction);

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

        // FIXME: VST3 GUI Challenges:
        // 1. No Floating Window Support - VST3 doesn't distinguish floating/embedded at create time.
        //    The isFloating parameter in create() cannot be directly mapped. Treating all as embedded.
        // 2. No Show/Hide API - VST3 IPlugView has no show/hide methods. We track state internally
        //    but cannot actually control visibility - that's the host window's responsibility.
        // 3. No Window Title API - VST3 IPlugView cannot set window titles. The setWindowTitle()
        //    API is a no-op; host must handle window titling.
        // 4. IPlugFrame Callback Routing - The HostApplication::resize_view() callback needs to
        //    route resize requests from the plugin back to the correct UISupport instance.
        //    Currently no mechanism to map from HostApplication to UISupport instance.
        class UISupport : public PluginUISupport {
            PluginInstanceVST3* owner;
            IPlugView* view{nullptr};
            IPlugViewContentScaleSupport* scale_support{nullptr};
            bool created{false};
            bool visible{false};
            bool attached{false};
            std::function<bool(uint32_t, uint32_t)> host_resize_handler{};
            FIDString target_ui_string{};

        public:
            explicit UISupport(PluginInstanceVST3* owner);
            ~UISupport() override = default;
            bool hasUI() override;
            bool create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) override;
            void destroy() override;
            bool show() override;
            void hide() override;
            void setWindowTitle(std::string title) override;
            bool canResize() override;
            bool getSize(uint32_t &width, uint32_t &height) override;
            bool setSize(uint32_t width, uint32_t height) override;
            bool suggestSize(uint32_t &width, uint32_t &height) override;
            bool setScale(double scale) override;
        };

        PluginFormatVST3Impl* owner;
        void* module;
        IPluginFactory* factory;
        std::string pluginName;
        IComponent* component;
        IAudioProcessor* processor;
        IEditController* controller;
        INoteExpressionController* note_expression_controller{nullptr};
        IUnitInfo* unit_info{nullptr};
        IProgramListData* program_list_data{nullptr};
        IMidiMapping* midi_mapping{nullptr};
        IMidiMapping2* midi_mapping2{nullptr};
        bool isControllerDistinctFromComponent;
        FUnknown* instance;
        // the connection point for IComponent, retrieved from the plugin.
        IConnectionPoint* connPointComp{nullptr};
        // the connection point for IEditController, retrieved from the plugin.
        IConnectionPoint* connPointEdit{nullptr};

        ProcessData processData{};
        ProcessContext process_context{};
        HostEventList processDataInputEvents{};
        HostEventList processDataOutputEvents{};
        HostParameterChanges processDataInputParameterChanges{};
        HostParameterChanges processDataOutputParameterChanges{};
        ProcessSetup last_process_setup{};
        bool has_process_setup{false};

        void allocateProcessData(ProcessSetup& setup);

        AudioBuses* audio_buses{};

        ParameterSupport* _parameters{};
        PluginStatesVST3* _states{};
        PluginPresetsSupport* _presets{};
        PluginUISupport* _ui{};

        VST3UmpInputDispatcher ump_input_dispatcher{this};
        bool componentActive{false};
        bool processingActive{false};

        // Cached MIDI mappings (retrieved on UI thread, used on audio thread)
        // From IMidiMapping2 (VST3.8.0+)
        std::vector<Midi1ControllerParamIDAssignment> cached_midi1_mappings_from_mapping2{};
        std::vector<Midi2ControllerParamIDAssignment> cached_midi2_mappings_from_mapping2{};
        // From IMidiMapping (pre-VST3.8.0)
        std::vector<Midi1ControllerParamIDAssignment> cached_midi1_mappings_from_mapping{};

        void refreshMidiMappings();

    public:
        explicit PluginInstanceVST3(
            PluginFormatVST3Impl* owner,
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
            return owner->requiresUIThreadOn(info());
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

        // ui
        PluginUISupport* ui() override;

    private:
        void handleRestartComponent(int32 flags);
        void synchronizeControllerState();
    };

}
