#pragma once

#ifdef __EMSCRIPTEN__

#include "remidy/remidy.hpp"
#include "remidy/priv/queued-state-operations.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace remidy {

    class PluginFormatWebCLAPImpl;
    struct WebClapParamDescriptor {
        uint32_t index{};
        std::string id;
        std::string name;
        std::string path;
        double minValue{};
        double maxValue{};
        double defaultValue{};
        double currentValue{};
        bool automatable{};
        bool hidden{};
        bool readOnly{};
        bool stepped{};
        bool automatablePerKey{};
        bool automatablePerChannel{};
        bool automatablePerPort{};
        bool modulatablePerKey{};
        bool modulatablePerChannel{};
        bool modulatablePerPort{};
        bool modulatablePerNoteId{};
    };

    struct WebClapCapabilities {
        bool hasEventInputs{};
        bool hasEventOutputs{};
        bool hasState{};
        bool hasPresetLoad{};
    };

    // ── Scanning ──────────────────────────────────────────────────────────────

    class PluginScanningWebCLAP : public FileOrUrlBasedPluginScanning {
        PluginFormatWebCLAPImpl* owner_;
    public:
        explicit PluginScanningWebCLAP(PluginFormatWebCLAPImpl* owner) : owner_(owner) {}
        ~PluginScanningWebCLAP() override = default;

        bool scanningMayBeSlow() override { return true; }
        bool usePluginSearchPaths() override { return false; }
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override {
            return ScanningStrategyValue::ALWAYS;
        }
        bool scanRequiresLoadLibrary(const std::filesystem::path&) override { return true; }
        ScanningStrategyValue scanRequiresInstantiation() override {
            return ScanningStrategyValue::ALWAYS;
        }

        std::vector<PluginCatalogEntry> getAllFastScannablePlugins() override;
        void scanBundle(const std::filesystem::path& bundlePath,
                        bool requireFastScanning,
                        double timeoutSeconds,
                        std::function<void(PluginCatalogEntry entry)> pluginFound,
                        PluginScanCompletedCallback scanCompleted) override;
        std::vector<std::filesystem::path> enumerateCandidateBundles(bool requireFastScanning) override;
        void startSlowPluginScan(std::function<void(PluginCatalogEntry entry)> pluginFound,
                                 PluginScanCompletedCallback scanCompleted) override;
    };

    // ── Instance ──────────────────────────────────────────────────────────────

    class PluginInstanceWebCLAP : public PluginInstance {

        class BusesWebCLAP : public PluginAudioBuses {
            PluginInstanceWebCLAP* owner_;
            AudioBusDefinition              out_def_{"main", AudioBusRole::Main,
                                                     {AudioChannelLayout::stereo()}};
            AudioBusConfiguration           out_cfg_{out_def_};
            std::vector<AudioBusConfiguration*> out_buses_{&out_cfg_};
            std::vector<AudioBusConfiguration*> in_buses_{};
        public:
            explicit BusesWebCLAP(PluginInstanceWebCLAP* owner) : owner_(owner) {}
            bool hasEventInputs() override;
            bool hasEventOutputs() override;
            const std::vector<AudioBusConfiguration*>& audioInputBuses()  const override { return in_buses_; }
            const std::vector<AudioBusConfiguration*>& audioOutputBuses() const override { return out_buses_; }
        };

        class ParamSupportWebCLAP : public PluginParameterSupport {
            PluginInstanceWebCLAP* owner_;
        public:
            explicit ParamSupportWebCLAP(PluginInstanceWebCLAP* owner) : owner_(owner) {}
            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes,
                                                              PerNoteControllerContext) override;
            StatusCode setParameter(uint32_t index, double plainValue, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double* plainValue) override;
            StatusCode setPerNoteController(PerNoteControllerContext, uint32_t index, double plainValue,
                                            uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext, uint32_t,
                                            double*) override;
            std::string valueToString(uint32_t index, double v) override;
            std::string valueToStringPerNote(PerNoteControllerContext, uint32_t,
                                             double v) override;
        };

        class StateSupportWebCLAP : public PluginStateSupport {
            PluginInstanceWebCLAP* owner_;
            QueuedStateOperationManager queue_{};
        public:
            explicit StateSupportWebCLAP(PluginInstanceWebCLAP* owner) : owner_(owner) {}
            bool requiresMainThread() override { return true; }
            std::vector<uint8_t> getState(StateContextType, bool) override;
            void setState(std::vector<uint8_t>&, StateContextType, bool) override;
            void requestState(StateContextType stateContextType, bool includeUiState, void* callbackContext,
                              std::function<void(std::vector<uint8_t> state, std::string error, void* callbackContext)> receiver) override;
            void loadState(std::vector<uint8_t> state, StateContextType stateContextType, bool includeUiState, void* callbackContext,
                           std::function<void(std::string error, void* callbackContext)> completed) override;
        };

        class PresetsSupportWebCLAP : public PluginPresetsSupport {
            PluginInstanceWebCLAP* owner_;
        public:
            explicit PresetsSupportWebCLAP(PluginInstanceWebCLAP* owner) : owner_(owner) {}
            bool isIndexStable() override { return false; }
            bool isIndexId() override { return false; }
            int32_t getPresetIndexForId(std::string&) override;
            int32_t getPresetCount() override;
            PresetInfo getPresetInfo(int32_t) override;
            void loadPreset(int32_t) override;
        };

        class UISupportWebCLAP : public PluginUISupport {
            PluginInstanceWebCLAP* owner_{};
            bool created_{false};
            bool visible_{false};
            bool can_resize_{false};
            uint32_t width_{800};
            uint32_t height_{600};
            std::string container_id_{};
            std::function<bool(uint32_t, uint32_t)> resize_handler_{};
        public:
            explicit UISupportWebCLAP(PluginInstanceWebCLAP* owner) : owner_(owner) {}
            bool hasUI() override;
            bool create(bool, void*, std::function<bool(uint32_t, uint32_t)>) override;
            void destroy() override;
            bool show() override;
            void hide() override;
            void setWindowTitle(std::string) override;
            bool canResize() override;
            bool getSize(uint32_t&, uint32_t&) override;
            bool setSize(uint32_t, uint32_t) override;
            bool suggestSize(uint32_t&, uint32_t&) override;
            bool setScale(double) override { return false; }
            void updateUiState(bool canResize, uint32_t width, uint32_t height);
        };

        uint32_t slot_;
        mutable std::mutex parameter_mutex_;
        std::vector<WebClapParamDescriptor> parameter_descriptors_{};
        std::vector<std::unique_ptr<PluginParameter>> parameter_defs_{};
        std::vector<PluginParameter*> parameter_ptrs_{};
        std::vector<PluginParameter*> per_note_parameter_ptrs_{};
        std::unordered_map<uint32_t, double> parameter_values_{};
        std::unique_ptr<BusesWebCLAP>        buses_{};
        std::unique_ptr<ParamSupportWebCLAP> params_{};
        std::unique_ptr<StateSupportWebCLAP> state_{};
        std::unique_ptr<PresetsSupportWebCLAP> presets_{};
        std::unique_ptr<UISupportWebCLAP>    ui_{};

    public:
        PluginInstanceWebCLAP(PluginCatalogEntry* entry, uint32_t slot);
        ~PluginInstanceWebCLAP() override;

        uint32_t slot() const { return slot_; }

        PluginUIThreadRequirement requiresUIThreadOn() override {
            return PluginUIThreadRequirement::None;
        }

        void updateParameters(const std::vector<WebClapParamDescriptor>& descriptors);
        void updateCapabilities(const WebClapCapabilities& capabilities);
        std::vector<PluginParameter*>& parameterPointers() { return parameter_ptrs_; }
        const std::vector<PluginParameter*>& parameterPointers() const { return parameter_ptrs_; }
        std::vector<PluginParameter*>& perNoteParameterPointers() { return per_note_parameter_ptrs_; }
        bool getCachedParameterValue(uint32_t index, double* plainValue) const;
        void setCachedParameterValue(uint32_t index, double plainValue);
        void applyParameterValueUpdate(uint32_t index, double plainValue);
        std::string buildParameterValueString(uint32_t index, double plainValue) const;
        void attachToTrackGraph(int32_t trackIndex, bool isMasterTrack, uint32_t order);
        bool hasUiSupport() const;
        void updateUiInfo(bool hasUi, bool canResize, uint32_t width, uint32_t height);
        void notifyUiResizeRequest(bool canResize, uint32_t width, uint32_t height);
        bool getUiSize(uint32_t& width, uint32_t& height) const;
        bool canUiResize() const;
        bool hasEventInputs() const { return has_event_inputs_; }
        bool hasEventOutputs() const { return has_event_outputs_; }
        bool hasStateSupport() const { return has_state_support_; }
        bool hasPresetLoadSupport() const { return has_preset_load_support_; }
        bool parameterSupportsContext(uint32_t index, PerNoteControllerContextTypes types) const;

        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;
        StatusCode process(AudioProcessContext& ctx) override;
        // The actual DSP runs in the AudioWorklet thread via wclap.mjs, which
        // mixes plugin output directly into master_output after the C++ engine
        // renders.  The C++ side acts as a transparent passthrough so that any
        // audio already in the signal chain (e.g. on the master track) is
        // preserved rather than zeroed by the graph's clearAudioOutputs().

        PluginAudioBuses* audioBuses() override {
            return (buses_ ? buses_ : buses_ = std::make_unique<BusesWebCLAP>(this)).get();
        }
        PluginParameterSupport* parameters() override;
        PluginStateSupport* states() override {
            return (state_ ? state_ : state_ = std::make_unique<StateSupportWebCLAP>(this)).get();
        }
        PluginPresetsSupport* presets() override {
            return (presets_ ? presets_ : presets_ = std::make_unique<PresetsSupportWebCLAP>(this)).get();
        }
        PluginUISupport* ui() override {
            return (ui_ ? ui_ : ui_ = std::make_unique<UISupportWebCLAP>(this)).get();
        }
        bool requiresReplacingProcess() const override { return false; }

    private:
        bool has_ui_{false};
        bool ui_can_resize_{false};
        uint32_t ui_width_{800};
        uint32_t ui_height_{600};
        bool has_event_inputs_{false};
        bool has_event_outputs_{false};
        bool has_state_support_{false};
        bool has_preset_load_support_{false};
    };

    // ── Format implementation ─────────────────────────────────────────────────

    class PluginFormatWebCLAPImpl : public PluginFormatWebCLAP {
    public:
        struct PendingRequest {
            PluginCatalogEntry* entry;
            uint32_t slot;
            std::function<void(std::unique_ptr<PluginInstance>, std::string)> callback;
        };
        struct PendingScanRequest {
            std::filesystem::path bundlePath;
            std::function<void(PluginCatalogEntry entry)> pluginFound;
            PluginScanCompletedCallback scanCompleted;
        };
        struct PendingStateRequest {
            uint32_t slot;
            std::function<void(std::vector<uint8_t> state, std::string error)> callback;
        };
        struct PendingStateLoadRequest {
            uint32_t slot;
            std::function<void(std::string error)> callback;
        };

    private:
        PluginScanningWebCLAP           scanning_{this};

    public:
        PluginFormatWebCLAPImpl();
        ~PluginFormatWebCLAPImpl() override;

        PluginScanning* scanning() override { return &scanning_; }
        void registerInstance(PluginInstanceWebCLAP* instance);
        void unregisterInstance(uint32_t slot);

        void createInstance(PluginCatalogEntry* info,
                            PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance>,
                                               std::string)> callback) override;
        void startBundleScan(const std::filesystem::path& bundlePath,
                             std::function<void(PluginCatalogEntry entry)> pluginFound,
                             PluginScanCompletedCallback scanCompleted);
        uint32_t reserveRequestId();
        void registerPendingStateRequest(uint32_t reqId, uint32_t slot,
                                         std::function<void(std::vector<uint8_t> state, std::string error)> callback);
        void registerPendingStateLoadRequest(uint32_t reqId, uint32_t slot,
                                             std::function<void(std::string error)> callback);

        void onBridgeMessage(const char* json) override;
        void onBridgeStateResponse(uint32_t reqId, const uint8_t* data, size_t size, const char* error);
        void onBridgeStateLoadComplete(uint32_t reqId, const char* error);
    };

} // namespace remidy

#endif // __EMSCRIPTEN__
