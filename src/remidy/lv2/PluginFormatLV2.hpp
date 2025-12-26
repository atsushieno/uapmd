#pragma once

#include <atomic>
#include <optional>
#include <unordered_map>

#include "remidy/remidy.hpp"
#include "../GenericAudioBuses.hpp"
#include "lilv/lilv.h"
#include <lv2/ui/ui.h>

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#endif

#include "LV2Helper.hpp"
#include "concurrentqueue.h"

namespace remidy {
    class PluginFormatLV2Impl;

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

    class PluginFormatLV2Impl : public PluginFormatLV2 {
        Logger *logger;
        PluginFormatLV2::Extensibility extensibility;
        AudioPluginScannerLV2 scanning_{nullptr};

    public:
        explicit PluginFormatLV2Impl(std::vector<std::string>& overrideSearchPaths);

        ~PluginFormatLV2Impl() override;

        auto getLogger() { return logger; }

        LilvWorld *world;
        remidy_lv2::LV2ImplWorldContext *worldContext;
        std::vector<LV2_Feature *> features{};

        PluginExtensibility<PluginFormat> *getExtensibility() override;

        PluginScanning *scanning() override { return &scanning_; }

        void createInstance(PluginCatalogEntry *info,
                            PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) override;

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
                : context(context), def(def), current(def->defaultPlainValue()) {

        }

        virtual ~LV2ParameterHandler() = default;

        virtual StatusCode setParameter(double value, remidy_timestamp_t timestamp) = 0;

        virtual StatusCode getParameter(double *value) {
            *value = current;
            return StatusCode::OK;
        }

        void updateCachedValue(double value) { current = value; }
    };

    class PluginInstanceLV2 : public PluginInstance {
        class ParameterSupport : public PluginParameterSupport {
            std::vector<PluginParameter *> parameter_defs{};
            std::vector<LV2ParameterHandler *> parameter_handlers{};
            std::unordered_map<LV2_URID, uint32_t> property_urid_to_index{};
            std::unordered_map<uint32_t, LV2_URID> index_to_property_urid{};

            void inspectParameters();

        public:
            explicit ParameterSupport(PluginInstanceLV2 *owner) : owner(owner) {
                inspectParameters();
            }

            ~ParameterSupport();

            PluginInstanceLV2 *owner;

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
            std::string valueToString(uint32_t index, double value) override;
            std::string valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) override {
                (void) context; (void) index; (void) value;
                return "";
            }
            void refreshParameterMetadata(uint32_t index) override;
            std::optional<uint32_t> indexForProperty(LV2_URID propertyUrid) const;
            std::optional<LV2_URID> propertyUridForIndex(uint32_t index) const;
            void updateCachedParameterValue(uint32_t index, double plainValue);
            void notifyParameterValue(uint32_t index, double plainValue) { notifyParameterChangeListeners(index, plainValue); }

            // Internal method for setting parameters with control over UI notification
            StatusCode setParameterInternal(uint32_t index, double value, uint64_t timestamp, bool notifyUI);
        };

        class LV2AtomParameterHandler : public LV2ParameterHandler {
            ParameterSupport* owner;

        public:
            LV2AtomParameterHandler(ParameterSupport* owner, remidy_lv2::LV2ImplPluginContext &context, PluginParameter *def)
                    : LV2ParameterHandler(context, def), owner(owner) {
            }

            ~LV2AtomParameterHandler() override = default;

            StatusCode setParameter(double value, remidy_timestamp_t timestamp) override;
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

            StatusCode getParameter(double *value) override {
                // Read the actual value from the port buffer
                if (!value)
                    return StatusCode::INVALID_PARAMETER_OPERATION;
                if (port_index < owner->owner->lv2_ports.size()) {
                    auto buffer = static_cast<float*>(owner->owner->lv2_ports[port_index].port_buffer);
                    if (buffer) {
                        *value = static_cast<double>(*buffer);
                        return StatusCode::OK;
                    }
                }
                return StatusCode::INVALID_PARAMETER_OPERATION;
            }

            StatusCode setParameter(double value, remidy_timestamp_t timestamp) override {
                // timestamp cannot be supported for ControlPort.
                if (port_index < owner->owner->lv2_ports.size()) {
                    auto buffer = static_cast<float*>(owner->owner->lv2_ports[port_index].port_buffer);
                    if (buffer) {
                        *buffer = static_cast<float>(value);
                        return StatusCode::OK;
                    }
                }
                return StatusCode::INVALID_PARAMETER_OPERATION;
            }
        };

        class LV2UmpInputDispatcher : public TypedUmpInputDispatcher {
            PluginInstanceLV2* owner;
            uint8_t midi1Bytes[16];

        public:
            LV2UmpInputDispatcher(PluginInstanceLV2* owner) : owner(owner), atom_context_group(0) {}

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
            void configure(ConfigurationRequest& config);
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

            // Notify UI of parameter changes
            void notifyParameterChange(LV2_URID propertyUrid, double value);

        private:
            PluginInstanceLV2* owner;

            // UI discovery and instance
            LilvUIs* available_uis{nullptr};
            const LilvUI* selected_ui{nullptr};
            const char* selected_ui_type{nullptr};
            void* ui_lib_handle{nullptr};
            const LV2UI_Descriptor* ui_descriptor{nullptr};
            LV2UI_Handle ui_handle{nullptr};
            LV2UI_Widget ui_widget{nullptr};

            // Optional interfaces
            const LV2UI_Idle_Interface* idle_interface{nullptr};
            const LV2UI_Show_Interface* show_interface{nullptr};
            const LV2UI_Resize* ui_resize_interface{nullptr};

            // State
            bool created{false};
            bool visible{false};
            bool is_floating{false};
            std::string window_title{};
            void* parent_widget{nullptr};

            // Host features
            LV2UI_Resize host_resize_feature{};
            LV2UI_Port_Map port_map_feature{};
            LV2UI_Port_Subscribe port_subscribe_feature{};
            std::function<bool(uint32_t, uint32_t)> host_resize_handler{};

#ifdef HAVE_WAYLAND
            // Wayland-specific state
            wl_surface* wayland_parent_surface{nullptr};
#endif

            // Idle timer state
            std::atomic<bool> idle_timer_running{false};

            // Internal helpers
            bool discoverUI();
            bool instantiateUI();
            void startIdleTimer();
            void stopIdleTimer();
            void scheduleIdleCallback();
            std::vector<const LV2_Feature*> buildFeatures();
            static void writeFunction(LV2UI_Controller controller, uint32_t port_index,
                                     uint32_t buffer_size, uint32_t port_protocol,
                                     const void* buffer);
            static uint32_t portIndex(LV2UI_Feature_Handle handle, const char* symbol);
            static int resizeUI(LV2UI_Feature_Handle handle, int width, int height);
        };

        PluginFormatLV2Impl *formatImpl;
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
        int32_t control_atom_port_index{-1};
        struct PendingParameterChange {
            uint32_t index;
            double value;
            remidy_timestamp_t timestamp;
        };
        moodycamel::ConcurrentQueue<PendingParameterChange> pending_parameter_changes{};

        struct PendingAtomEvent {
            uint32_t port_index;
            uint32_t buffer_size;
            uint32_t port_protocol;
            std::vector<uint8_t> buffer;
        };
        moodycamel::ConcurrentQueue<PendingAtomEvent> pending_atom_events{};

        std::atomic<bool> in_audio_process{false};
        std::atomic<bool> processing_requested_{false};
        bool processing_active_{false};

        struct RemidyToLV2PortMapping {
            size_t bus;
            uint32_t channel;
            int32_t lv2Port;
        };
        std::vector<RemidyToLV2PortMapping> audio_in_port_mapping{};
        std::vector<RemidyToLV2PortMapping> audio_out_port_mapping{};
        std::vector<std::vector<float>> audio_in_fallback_buffers{};
        std::vector<std::vector<float>> audio_out_fallback_buffers{};

        AudioBuses* audio_buses{};

        ParameterSupport *_parameters{};
        PluginStateSupport *_states{};
        PluginPresetsSupport *_presets{};
        PluginUISupport *_ui{};

        LV2UmpInputDispatcher ump_input_dispatcher{this};
        void enqueueParameterChange(uint32_t index, double value, remidy_timestamp_t timestamp);
        void flushPendingParameterChanges();
        void enqueueAtomEvent(uint32_t port_index, uint32_t buffer_size, uint32_t port_protocol, const void* buffer);
        void flushPendingAtomEvents();

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
        explicit PluginInstanceLV2(PluginCatalogEntry *entry, PluginFormatLV2Impl *formatImpl,
                                   const LilvPlugin *plugin);

        ~PluginInstanceLV2() override;

        PluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return formatImpl->requiresUIThreadOn(info());
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

    inline StatusCode PluginInstanceLV2::LV2AtomParameterHandler::setParameter(double value, remidy_timestamp_t timestamp) {
        current = value;
        owner->owner->enqueueParameterChange(def->index(), value, timestamp);
        return StatusCode::OK;
    }
}
