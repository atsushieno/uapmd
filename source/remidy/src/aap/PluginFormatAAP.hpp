#pragma once

#include "remidy/remidy.hpp"
#include "remidy/detail/queued-state-operations.hpp"
#include <stdexcept>
#include <aap/core/host/plugin-connections.h>
#include <aap/core/host/plugin-host.h>
#include <aap/core/host/plugin-instance.h>

#if defined(__ANDROID__)
#include <jni.h>
namespace remidy::gui::android {
    void attachSurfaceView(void* windowHandle, jobject surfaceView);
    void detachSurfaceView(void* windowHandle);
    void queryDimensions(void* windowHandle, int& width, int& height);
}
#endif

namespace remidy {
    class PluginFormatAAPImpl;
    class PluginInstanceAAP;

    class PluginScanningAAP : public PluginScanning {
        PluginFormatAAP* owner;

    public:
        PluginScanningAAP(PluginFormatAAP* owner) : owner(owner) {}
        ~PluginScanningAAP() override = default;

        bool scanningMayBeSlow() override { return false; }
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
        bool scanRequiresLoadLibrary(const std::filesystem::path&) override { return false; }

        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }

        std::vector<PluginCatalogEntry> getAllFastScannablePlugins() override;
        void startSlowPluginScan(std::function<void(PluginCatalogEntry entry)> pluginFound,
                                 PluginScanCompletedCallback scanCompleted) override;
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
            std::vector<double> parameter_values{};

        public:
            ParameterSupport(PluginInstanceAAP* owner);
            ~ParameterSupport() override;

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

            void ingestPluginParameterUpdates(const uint8_t* data, size_t lengthInBytes);

        };

        class PresetsSupport : public PluginPresetsSupport {
            remidy::PluginInstanceAAP* owner;

        public:
            PresetsSupport(remidy::PluginInstanceAAP* owner) : owner(owner) {}

            bool isIndexStable() override {
                return true;
            }

            bool isIndexId() override {
                return true;
            }

            int32_t getPresetIndexForId(std::string &id) override {
                return std::stol(id);
            }

            int32_t getPresetCount() override {
                return owner->aapInstance()->getStandardExtensions().getPresetCount();
            }

            PresetInfo getPresetInfo(int32_t index) override {
                aap_preset_t preset;
                owner->aapInstance()->getStandardExtensions().getPreset(index, preset);
                return PresetInfo{std::to_string(preset.id), preset.name, 0, index};
            }

            void loadPreset(int32_t index) override {
                owner->aapInstance()->getStandardExtensions().setCurrentPresetIndex(index);
            }
        };

        class UISupport : public PluginUISupport {
            remidy::PluginInstanceAAP* owner;
            aap_gui_instance_id  gui_instance_id{-1};
            void* parent_handle_{nullptr};
            std::function<bool(uint32_t, uint32_t)> host_resize_handler_{};
#if defined(__ANDROID__)
            bool surface_attached_{false};
            uint32_t surface_width_{800};
            uint32_t surface_height_{600};
#endif

        public:
            UISupport(remidy::PluginInstanceAAP* owner) : owner(owner) {}

            bool hasUI() override {
                // FIXME: implement
                return true;
            }

            bool create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) override {
                (void)isFloating;
                if (gui_instance_id >= 0)
                    return false;
                parent_handle_ = parentHandle;
                host_resize_handler_ = std::move(resizeHandler);
                auto aap = owner->aapInstance();
                if (!aap)
                    return false;
#if defined(__ANDROID__)
                auto remote = dynamic_cast<aap::RemotePluginInstance*>(aap);
                if (!remote || parent_handle_ == nullptr)
                    return false;
                remote->prepareSurfaceControlForRemoteNativeUI();
                auto nativeView = static_cast<jobject>(remote->getRemoteNativeView());
                if (!nativeView)
                    return false;
                remidy::gui::android::attachSurfaceView(parent_handle_, nativeView);
                surface_attached_ = true;
                int width = static_cast<int>(surface_width_);
                int height = static_cast<int>(surface_height_);
                remidy::gui::android::queryDimensions(parent_handle_, width, height);
                surface_width_ = static_cast<uint32_t>(width);
                surface_height_ = static_cast<uint32_t>(height);
                remote->connectRemoteNativeView(width, height);
                gui_instance_id = 0;
                return true;
#else
                gui_instance_id = aap->getStandardExtensions().createGui(aap->getPluginInformation()->getPluginID(), aap->getInstanceId(), parentHandle);
                return gui_instance_id >= 0;
#endif
            }

            void destroy() override {
#if defined(__ANDROID__)
                if (surface_attached_ && parent_handle_) {
                    remidy::gui::android::detachSurfaceView(parent_handle_);
                    surface_attached_ = false;
                }
                parent_handle_ = nullptr;
                host_resize_handler_ = {};
                gui_instance_id = -1;
#else
                if (gui_instance_id < 0)
                    return;

                auto aap = owner->aapInstance();
                aap->getStandardExtensions().destroyGui(gui_instance_id);
                gui_instance_id = -1;
#endif
            }

            bool show() override {
#if defined(__ANDROID__)
                return gui_instance_id >= 0;
#else
                if (gui_instance_id < 0)
                    return false;
                auto aap = owner->aapInstance();
                aap->getStandardExtensions().showGui(gui_instance_id);
                return true;
#endif
            }

            void hide() override {
#if defined(__ANDROID__)
                // Visibility is controlled by the host container window.
                (void)0;
#else
                if (gui_instance_id < 0)
                    return;
                auto aap = owner->aapInstance();
                aap->getStandardExtensions().hideGui(gui_instance_id);
#endif
            }

            void setWindowTitle(std::string title) override {
                // AAP does not support it
            }

            bool canResize()  override {
                return true; // AAP has no option
            }

            bool getSize(uint32_t &width, uint32_t &height)  override {
#if defined(__ANDROID__)
                width = surface_width_;
                height = surface_height_;
                return true;
#else
                return false;
#endif
            }

            bool setSize(uint32_t width, uint32_t height)  override {
#if defined(__ANDROID__)
                surface_width_ = width;
                surface_height_ = height;
                return true;
#else
                if (gui_instance_id < 0)
                    return false;
                auto aap = owner->aapInstance();
                aap->getStandardExtensions().resizeGui(gui_instance_id, width, height);
                return true;
#endif
            }

            bool suggestSize(uint32_t &width, uint32_t &height)  override {
                // AAP does not support it
                return false;
            }

            bool setScale(double scale)  override {
                // AAP does not support it
                return false;
            }
        };

        class StateSupport : public PluginStateSupport {
            remidy::PluginInstanceAAP* owner;
            QueuedStateOperationManager queue_{};

        public:
            StateSupport(remidy::PluginInstanceAAP* owner) : owner(owner) {}

            bool requiresMainThread() override { return false; }
            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override;
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override;
            void requestState(StateContextType stateContextType, bool includeUiState, void* callbackContext,
                              std::function<void(std::vector<uint8_t> state, std::string error, void* callbackContext)> receiver) override;
            void loadState(std::vector<uint8_t> state, StateContextType stateContextType, bool includeUiState, void* callbackContext,
                           std::function<void(std::string error, void* callbackContext)> completed) override;
        };

        PluginFormatAAPImpl* format;
        std::unique_ptr<PluginBusesAAP> buses{};
        std::unique_ptr<ParameterSupport> params{};
        std::unique_ptr<PresetsSupport> presets_{};
        std::unique_ptr<StateSupport> state_{};
        std::unique_ptr<UISupport> ui_{};
        aap::PluginInstance* instance;

        std::vector<int32_t> remidy_to_aap_port_index_map_audio_in{};
        std::vector<int32_t> remidy_to_aap_port_index_map_audio_out{};
        int32_t aap_port_midi2_in{-1};
        int32_t aap_port_midi2_out{-1};

    public:
        PluginInstanceAAP(PluginFormatAAPImpl* format, PluginCatalogEntry* entry, aap::PluginInstance* aapInstance);

        aap::PluginInstance* aapInstance() { return instance; };

        PluginUIThreadRequirement requiresUIThreadOn() override { return PluginUIThreadRequirement::None; }

        StatusCode configure(ConfigurationRequest &configuration) override;

        StatusCode startProcessing() override;

        StatusCode stopProcessing() override;

        StatusCode process(AudioProcessContext &process) override;

        PluginAudioBuses *audioBuses() override {
            return (buses ? buses : buses = std::make_unique<PluginBusesAAP>(this)).get();
        }

        PluginParameterSupport *parameters() override {
            return (params ? params : params = std::make_unique<ParameterSupport>(this)).get();
        }

        PluginStateSupport *states() override {
            return (state_ ? state_ : state_ = std::make_unique<StateSupport>(this)).get();
        }

        PluginPresetsSupport *presets() override {
            return (presets_ ? presets_ : presets_ = std::make_unique<PresetsSupport>(this)).get();
        }

        PluginUISupport *ui() override {
            return (ui_ ? ui_ : ui_ = std::make_unique<UISupport>(this)).get();
        }

        bool requiresReplacingProcess() const override { return false; }
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
