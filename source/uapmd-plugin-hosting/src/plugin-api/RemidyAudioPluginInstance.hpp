#pragma once

#include <memory>
#include <mutex>

#include "remidy/remidy.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"
#ifdef UAPMD_HAS_ARA
#include <uapmd-ara/ara-plugin-instance-handles.hpp>
#endif

// The AudioPluginInstanceAPI implementation for the plugin formats that remidy provides.
// It is private to the module: an application-provided AudioPluginFormat implements
// AudioPluginInstanceAPI directly instead of going through this.

namespace uapmd_plugin_hosting {

    remidy::PluginStateSupport::StateContextType toRemidyStateContextType(StateContextType type);

    // Some operations (loading a preset or a state, setting a parameter) can make a plugin
    // report a different latency or tail, which the host has to react to.
    void notifyTimingInfoChangeIfNeeded(remidy::PluginInstance& instance,
                                        uint32_t previousLatency,
                                        double previousTail);

    class RemidyAudioPluginInstance : public AudioPluginInstanceAPI {
#ifdef UAPMD_HAS_ARA
        class AraHandleExtensionAdapter final
            : public AudioPluginInstanceExtension
            , public uapmd::ara::AraPluginInstanceHandleExtension {
            RemidyAudioPluginInstance& owner;

        public:
            explicit AraHandleExtensionAdapter(RemidyAudioPluginInstance& owner)
                : owner(owner) {
            }

            std::string_view extensionId() const override {
                return uapmd::ara::kAraPluginInstanceHandleExtensionId;
            }

            void* nativeHandle(uapmd::ara::AraPluginInstanceHandleKind kind) const override {
                if (!owner.instance)
                    return nullptr;
                auto* extension = dynamic_cast<uapmd::ara::AraPluginInstanceHandleExtension*>(
                    owner.instance->getExtensibility(uapmd::ara::kAraPluginInstanceHandleExtensionId));
                if (!extension)
                    return nullptr;
                return extension->nativeHandle(kind);
            }
        };
#endif

        class PluginStateChangeExtensionAdapter final : public PluginStateChangeExtension {
            RemidyAudioPluginInstance& owner;
            remidy::EventListenerId listener_id_{0};

        public:
            explicit PluginStateChangeExtensionAdapter(RemidyAudioPluginInstance& owner)
                : owner(owner) {
            }

            ~PluginStateChangeExtensionAdapter() override {
                if (owner.instance && listener_id_ != 0)
                    owner.instance->pluginStateChangeEvent().removeListener(listener_id_);
            }

            void onPluginStateChanged(std::function<void()> handler) override {
                if (!owner.instance)
                    return;
                if (listener_id_ != 0)
                    owner.instance->pluginStateChangeEvent().removeListener(listener_id_);
                listener_id_ = handler
                    ? owner.instance->pluginStateChangeEvent().addListener(std::move(handler))
                    : 0;
            }
        };

        class AapUiHostDetailsExtensionAdapter final : public AapUiHostDetailsExtension {
            RemidyAudioPluginInstance& owner;

        public:
            explicit AapUiHostDetailsExtensionAdapter(RemidyAudioPluginInstance& owner)
                : owner(owner) {
            }

            remidy::PluginInstanceAAPExt* aapExtensibility() const override {
                if (!owner.instance)
                    return nullptr;
                return dynamic_cast<remidy::PluginInstanceAAPExt*>(
                    owner.instance->getExtensibility(remidy::kAAPPluginInstanceExtensionId));
            }
        };

        bool bypassed_{true};
        struct AsyncPresetState {
            std::mutex mutex;
            remidy::PluginInstance* instance{};
        };
        std::shared_ptr<AsyncPresetState> async_preset_state_{std::make_shared<AsyncPresetState>()};
        std::unique_ptr<remidy::PluginInstance> owned_instance_{};
        remidy::PluginInstance* instance{};
#ifdef UAPMD_HAS_ARA
        AraHandleExtensionAdapter ara_handle_extension{*this};
#endif

        PluginStateChangeExtensionAdapter plugin_state_change_extension{*this};
        AapUiHostDetailsExtensionAdapter aap_ui_host_details_extension{*this};
        remidy::PluginUISupport* ui_support{nullptr};
        bool uiCreated{false};
        bool uiVisible{false};
        bool uiFloating{true};

        remidy::PluginUISupport* ensureUISupport() {
            if (!instance)
                return nullptr;
            if (!ui_support)
                ui_support = instance->ui();
            return ui_support;
        }

    public:
        explicit RemidyAudioPluginInstance(std::unique_ptr<remidy::PluginInstance> instance)
          : owned_instance_(std::move(instance)), instance(owned_instance_.get()) {
            bypassed_ = false;
            async_preset_state_->instance = this->instance;
        }
        ~RemidyAudioPluginInstance() override {
            {
                std::lock_guard lock(async_preset_state_->mutex);
                async_preset_state_->instance = nullptr;
            }
            bypassed_ = true;
            if (ui_support) {
                if (uiVisible)
                    ui_support->hide();
                if (uiCreated)
                    ui_support->destroy();
                uiVisible = false;
                uiCreated = false;
                uiFloating = true;
            }
        }

        bool bypassed() const override { return bypassed_; }
        void bypassed(bool value) override {
            bypassed_ = value;
            // For WebCLAP the actual DSP runs in the AudioWorklet thread and is
            // gated by info.active, which is not touched by the C++ bypassed_ flag.
            // Relay the state change so the worklet stops/resumes generating audio.
            if (instance && instance->info() && instance->info()->format() == "WebCLAP") {
                if (value)
                    instance->stopProcessing();
                else
                    instance->startProcessing();
            }
        }

        uapmd_status_t startProcessing() override {
            if (!instance)
                return -1;
            return static_cast<uapmd_status_t>(instance->startProcessing());
        }

        uapmd_status_t stopProcessing() override {
            if (!instance)
                return -1;
            return static_cast<uapmd_status_t>(instance->stopProcessing());
        }

        uapmd_status_t processAudio(remidy::AudioProcessContext &process) override {
            if (bypassed_)
                return 0;

            const bool replacing = instance && instance->requiresReplacingProcess();
            if (replacing) {
                process.copyInputsToOutputs();
                process.enableReplacingIO();
            }

            // FIXME: define error codes
            uapmd_status_t status = 0;
            if (const auto p = instance)
                status = static_cast<uapmd_status_t>(p->process(process));

            if (replacing)
                process.disableReplacingIO();
            return status;
        }

#ifdef __EMSCRIPTEN__
        bool trySendWebClapInputEvents(const uapmd_ump_t* events, size_t sizeInBytes) {
            if (auto* webclap = dynamic_cast<remidy::PluginInstanceWebCLAPControl*>(instance))
                return webclap->sendUmpInputEvents(events, sizeInBytes);
            return false;
        }
#endif

        uint32_t latencyInSamples() const override {
            return instance ? instance->latencyInSamples() : 0;
        }

        double tailLengthInSeconds() const override {
            return instance ? instance->tailLengthInSeconds() : 0.0;
        }

        std::vector<ParameterMetadata> parameterMetadataList() override {
            std::vector<ParameterMetadata> ret{};
            auto pl = instance->parameters();
            for (auto p : pl->parameters()) {
                std::vector<ParameterNamedValue> enums{};
                for (auto e : p->enums())
                    enums.emplace_back(ParameterNamedValue{
                        .value = e.value,
                        .name = e.label
                    });
                ret.emplace_back(ParameterMetadata{
                        .index = p->index(),
                        .stableId = p->stableId(),
                        .name = p->name(),
                        .path = p->path(),
                        .defaultPlainValue = p->defaultPlainValue(),
                        .minPlainValue = p->minPlainValue(),
                        .maxPlainValue = p->maxPlainValue(),
                        .automatable = p->automatable(),
                        .hidden = p->hidden(),
                        .discrete = p->discrete(),
                        .namedValues = std::vector(enums)
                });
            }
            return ret;
        }
        std::vector<ParameterMetadata> perNoteControllerMetadataList(remidy::PerNoteControllerContextTypes contextType, uint32_t context) override {
            if (contextType != remidy::PER_NOTE_CONTROLLER_PER_NOTE)
                return {};
            std::vector<ParameterMetadata> ret{};
            auto pl = instance->parameters();
            for (auto p : pl->perNoteControllers(contextType, { .note = context })) {
                std::vector<ParameterNamedValue> enums{};
                for (auto e : p->enums())
                    enums.emplace_back(ParameterNamedValue{
                        .value = e.value,
                        .name = e.label
                    });
                ret.emplace_back(ParameterMetadata{
                        .index = p->index(),
                        .stableId = p->stableId(),
                        .name = p->name(),
                        .path = p->path(),
                        .defaultPlainValue = p->defaultPlainValue(),
                        .minPlainValue = p->minPlainValue(),
                        .maxPlainValue = p->maxPlainValue(),
                        .automatable = p->automatable(),
                        .hidden = p->hidden(),
                        .discrete = p->discrete(),
                        .namedValues = std::vector(enums)
                });
            }
            return ret;
        }
        std::vector<PresetsMetadata> presetMetadataList() override {
            std::vector<PresetsMetadata> ret{};
            auto pl = instance->presets();
            for (int32_t p = 0, n = pl->getPresetCount(); p < n; p++) {
                auto info = pl->getPresetInfo(p);
                ret.emplace_back(PresetsMetadata {
                    .bank = static_cast<uint8_t>(info.bank()),
                    .index = static_cast<uint32_t>(info.index()),
                    .stableId = info.id(),
                    .name = info.name(),
                    .path = "" // FIXME: implement
                });
            }
            return ret;
        }

        std::string& displayName() const override { return instance->info()->displayName(); }
        std::string& formatName() const override { return instance->info()->format(); }
        std::string& pluginId() const override { return instance->info()->pluginId(); }

        void loadPreset(int32_t presetIndex) override {
            const auto previousLatency = instance->latencyInSamples();
            const auto previousTail = instance->tailLengthInSeconds();
            instance->presets()->loadPreset(presetIndex);
            notifyTimingInfoChangeIfNeeded(*instance, previousLatency, previousTail);
        }

        void loadPreset(int32_t presetIndex, std::function<void(std::string error, void* callbackContext)> completed) override {
            const auto previousLatency = instance->latencyInSamples();
            const auto previousTail = instance->tailLengthInSeconds();
            instance->presets()->loadPreset(presetIndex, [state = async_preset_state_, previousLatency, previousTail, completed = std::move(completed)](std::string error, void* callbackContext) mutable {
                {
                    std::lock_guard lock(state->mutex);
                    if (error.empty() && state->instance)
                        notifyTimingInfoChangeIfNeeded(*state->instance, previousLatency, previousTail);
                }
                if (completed)
                    completed(std::move(error), callbackContext);
            });
        }

        std::vector<uint8_t> saveStateSync() override {
            return instance->states()->getState(remidy::PluginStateSupport::StateContextType::Project, false);
        }

        void loadStateSync(std::vector<uint8_t> &state) override {
            const auto previousLatency = instance->latencyInSamples();
            const auto previousTail = instance->tailLengthInSeconds();
            instance->states()->setState(state, remidy::PluginStateSupport::StateContextType::Project, false);
            notifyTimingInfoChangeIfNeeded(*instance, previousLatency, previousTail);
        }

        void requestState(StateContextType stateContextType, bool includeUiState, void* callbackContext,
                          std::function<void(std::vector<uint8_t> state, std::string error, void* callbackContext)> receiver) override {
            instance->states()->requestState(toRemidyStateContextType(stateContextType), includeUiState, callbackContext, std::move(receiver));
        }

        void loadState(std::vector<uint8_t> state, StateContextType stateContextType, bool includeUiState, void* callbackContext,
                       std::function<void(std::string error, void* callbackContext)> completed) override {
            const auto previousLatency = instance->latencyInSamples();
            const auto previousTail = instance->tailLengthInSeconds();
            instance->states()->loadState(
                std::move(state),
                toRemidyStateContextType(stateContextType),
                includeUiState,
                callbackContext,
                [this, previousLatency, previousTail, completed = std::move(completed)](std::string error, void* callbackContext) mutable {
                    if (error.empty())
                        notifyTimingInfoChangeIfNeeded(*instance, previousLatency, previousTail);
                    if (completed)
                        completed(std::move(error), callbackContext);
                });
        }

        double getParameterValue(int32_t index) override {
            double value = 0.0;
            instance->parameters()->getParameter(index, &value);
            return value;
        }

        void setParameterValue(int32_t index, double value) override {
            const auto previousLatency = instance->latencyInSamples();
            const auto previousTail = instance->tailLengthInSeconds();
            instance->parameters()->setParameter(index, value);
            notifyTimingInfoChangeIfNeeded(*instance, previousLatency, previousTail);
        }

        void enqueueParameterValueRT(int32_t index, double value, uapmd_timestamp_t timestamp) override {
            instance->parameters()->enqueueParameterRT(index, value, timestamp);
        }

        std::string getParameterValueString(int32_t index, double value) override {
            return instance->parameters()->valueToString(index, value);
        }

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override {
            const auto previousLatency = instance->latencyInSamples();
            const auto previousTail = instance->tailLengthInSeconds();
            instance->parameters()->setPerNoteController({.note = note }, index, value);
            notifyTimingInfoChangeIfNeeded(*instance, previousLatency, previousTail);
        }

        bool getPerNoteControllerValue(uint8_t note, uint8_t index, double* value) override {
            if (!value)
                return false;
            return instance->parameters()->getPerNoteController(
                       {.note = note},
                       index,
                       value)
                == remidy::StatusCode::OK;
        }

        void enqueuePerNoteControllerValueRT(uint8_t note, uint8_t index, double value, uapmd_timestamp_t timestamp) override {
            instance->parameters()->enqueuePerNoteControllerRT({.note = note }, index, value, timestamp);
        }

        std::string getPerNoteControllerValueString(uint8_t note, uint8_t index, double value) override {
            return instance->parameters()->valueToStringPerNote({ .note = note }, index, value);
        }

        remidy::PluginInstance* rawInstance() const { return instance; }

        remidy::StatusCode configure(remidy::PluginInstance::ConfigurationRequest& configuration) {
            if (!instance)
                return remidy::StatusCode::ALREADY_INVALID_STATE;
            return instance->configure(configuration);
        }

        bool hasUISupport() override {
            auto ui = ensureUISupport();
            if (!ui)
                return false;
            return ui->hasUI();
        }

        bool createUI(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) override {
            auto ui = ensureUISupport();
            if (!ui)
                return false;

            // UI must not be created twice - call destroyUI() first
            if (uiCreated)
                return false;

            // Pass parent and resize handler to create() - they're immutable
            if (!ui->create(isFloating, parentHandle, resizeHandler))
                return false;

            uiCreated = true;
            uiFloating = isFloating;
            return true;
        }

        void destroyUI() override {
            if (!uiCreated)
                return;

            auto ui = ensureUISupport();
            if (!ui)
                return;

            if (uiVisible)
                ui->hide();
            ui->destroy();
            uiCreated = false;
            uiVisible = false;
        }

        bool showUI() override {
            auto ui = ensureUISupport();
            if (!ui)
                return false;
            // UI must be created first via createUI() - don't create here
            if (!uiCreated)
                return false;
            if (uiVisible)
                return true;
            if (!ui->show())
                return false;
            uiVisible = true;
            return true;
        }

        void hideUI() override {
            if (!ui_support || !uiVisible)
                return;
            ui_support->hide();
            uiVisible = false;
        }

        bool isUIVisible() const override {
            return uiVisible;
        }

        bool setUISize(uint32_t width, uint32_t height) override {
            auto ui = ensureUISupport();
            if (!ui || !uiCreated)
                return false;
            return ui->setSize(width, height);
        }

        bool getUISize(uint32_t &width, uint32_t &height) override {
            auto ui = ensureUISupport();
            if (!ui)
                return false;
            return ui->getSize(width, height);
        }

        bool canUIResize() override {
            auto ui = ensureUISupport();
            if (!ui || !uiCreated)
                return false;
            return ui->canResize();
        }


        remidy::PluginParameterSupport* parameterSupport() override {
            if (!instance)
                return nullptr;
            return instance->parameters();
        }

        remidy::PluginAudioBuses* audioBuses() override {
            if (!instance)
                return nullptr;
            return instance->audioBuses();
        }

        remidy::EventListenerId addTimingInfoChangeListener(
            std::function<void(remidy::PluginTimingInfoChange)> listener) override {
            if (!instance)
                return 0;
            return instance->timingInfoChangeEvent().addListener(std::move(listener));
        }

        void removeTimingInfoChangeListener(remidy::EventListenerId listenerId) override {
            if (instance)
                instance->timingInfoChangeEvent().removeListener(listenerId);
        }

        bool requiresReplacingProcess() const override {
            return instance && instance->requiresReplacingProcess();
        }

        AudioPluginInstanceExtension* extension(std::string_view extensionId) override {
            if (!instance)
                return nullptr;
#ifdef UAPMD_HAS_ARA
            if (extensionId == uapmd::ara::kAraPluginInstanceHandleExtensionId)
                return &ara_handle_extension;
#endif
            if (extensionId == kPluginStateChangeExtensionId)
                return &plugin_state_change_extension;
            if (extensionId == kAapUiHostDetailsExtensionId)
                return &aap_ui_host_details_extension;
            auto* remidyExtension = instance->getExtensibility(extensionId);
            if (!remidyExtension)
                return nullptr;
            return dynamic_cast<AudioPluginInstanceExtension*>(remidyExtension);
        }
    };



}
