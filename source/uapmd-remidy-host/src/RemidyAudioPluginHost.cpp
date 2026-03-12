
#include "uapmd-remidy-host/RemidyAudioPluginHost.hpp"
#include <functional>
#include <memory>
#include <ranges>
#if _WIN32
#include <Windows.h>
#endif

#include "remidy-tooling/PluginInstancing.hpp"
#include "midi/UapmdNodeUmpMapper.hpp"

namespace uapmd {
    namespace {
        ParameterMetadata toParameterMetadata(remidy::PluginParameter* param) {
            if (!param)
                return {};
            std::vector<ParameterNamedValue> enums{};
            enums.reserve(param->enums().size());
            for (auto e : param->enums()) {
                enums.emplace_back(ParameterNamedValue{
                    .value = e.value,
                    .name = e.label
                });
            }
            return ParameterMetadata{
                .index = param->index(),
                .stableId = param->stableId(),
                .name = param->name(),
                .path = param->path(),
                .defaultPlainValue = param->defaultPlainValue(),
                .minPlainValue = param->minPlainValue(),
                .maxPlainValue = param->maxPlainValue(),
                .automatable = param->automatable(),
                .hidden = param->hidden(),
                .discrete = param->discrete(),
                .namedValues = std::move(enums)
            };
        }

        ParameterValueStatus toParameterValueStatus(remidy::StatusCode code) {
            using remidy::StatusCode;
            switch (code) {
                case StatusCode::OK:
                    return ParameterValueStatus::Ok;
                case StatusCode::INVALID_PARAMETER_OPERATION:
                    return ParameterValueStatus::InvalidIndex;
                case StatusCode::NOT_IMPLEMENTED:
                    return ParameterValueStatus::Unsupported;
                default:
                    return ParameterValueStatus::BackendError;
            }
        }

        PerNoteContextFlags fromRemidyFlags(remidy::PerNoteControllerContextTypes flags) {
            PerNoteContextFlags result = PerNoteContextFlags::None;
            if ((flags & remidy::PER_NOTE_CONTROLLER_PER_CHANNEL) != 0)
                result = result | PerNoteContextFlags::PerChannel;
            if ((flags & remidy::PER_NOTE_CONTROLLER_PER_NOTE) != 0)
                result = result | PerNoteContextFlags::PerNote;
            if ((flags & remidy::PER_NOTE_CONTROLLER_PER_GROUP) != 0)
                result = result | PerNoteContextFlags::PerGroup;
            if ((flags & remidy::PER_NOTE_CONTROLLER_PER_EXTRA) != 0)
                result = result | PerNoteContextFlags::PerExtra;
            return result;
        }

        remidy::PerNoteControllerContextTypes toRemidyFlags(PerNoteContextFlags flags) {
            remidy::PerNoteControllerContextTypes result = remidy::PER_NOTE_CONTROLLER_NONE;
            if ((flags & PerNoteContextFlags::PerChannel) != PerNoteContextFlags::None)
                result = static_cast<remidy::PerNoteControllerContextTypes>(result | remidy::PER_NOTE_CONTROLLER_PER_CHANNEL);
            if ((flags & PerNoteContextFlags::PerNote) != PerNoteContextFlags::None)
                result = static_cast<remidy::PerNoteControllerContextTypes>(result | remidy::PER_NOTE_CONTROLLER_PER_NOTE);
            if ((flags & PerNoteContextFlags::PerGroup) != PerNoteContextFlags::None)
                result = static_cast<remidy::PerNoteControllerContextTypes>(result | remidy::PER_NOTE_CONTROLLER_PER_GROUP);
            if ((flags & PerNoteContextFlags::PerExtra) != PerNoteContextFlags::None)
                result = static_cast<remidy::PerNoteControllerContextTypes>(result | remidy::PER_NOTE_CONTROLLER_PER_EXTRA);
            return result;
        }

        remidy::PerNoteControllerContext toRemidyContext(const PerNoteContext& ctx) {
            remidy::PerNoteControllerContext remidyCtx{};
            remidyCtx.note = ctx.note;
            remidyCtx.channel = ctx.channel;
            remidyCtx.group = ctx.group;
            remidyCtx.extra = ctx.extra;
            return remidyCtx;
        }

        class RemidyParameterSupportAdapter {
        public:
            explicit RemidyParameterSupportAdapter(remidy::PluginParameterSupport* support) : support_(support) {}

            ParameterListenerId addParameterValueListener(ParameterChangeCallback cb) {
                if (!support_ || !cb)
                    return 0;
                return support_->parameterChangeEvent().addListener(
                    [cb = std::move(cb)](uint32_t index, double value) {
                        cb(index, value);
                    });
            }

            void removeParameterValueListener(ParameterListenerId id) {
                if (!support_)
                    return;
                support_->parameterChangeEvent().removeListener(id);
            }

            ParameterListenerId addParameterMetadataListener(ParameterMetadataCallback cb) {
                if (!support_ || !cb)
                    return 0;
                return support_->parameterMetadataChangeEvent().addListener(
                    [cb = std::move(cb)]() {
                        cb();
                    });
            }

            void removeParameterMetadataListener(ParameterListenerId id) {
                if (!support_)
                    return;
                support_->parameterMetadataChangeEvent().removeListener(id);
            }

            ParameterListenerId addPerNoteControllerListener(PerNoteControllerChangeCallback cb) {
                if (!support_ || !cb)
                    return 0;
                return support_->perNoteControllerChangeEvent().addListener(
                    [cb = std::move(cb)](remidy::PerNoteControllerContextTypes types, uint32_t context, uint32_t parameterIndex, double value) {
                        PerNoteContext ctx{};
                        if ((types & remidy::PER_NOTE_CONTROLLER_PER_NOTE) != 0)
                            ctx.note = context;
                        if ((types & remidy::PER_NOTE_CONTROLLER_PER_CHANNEL) != 0)
                            ctx.channel = context;
                        if ((types & remidy::PER_NOTE_CONTROLLER_PER_GROUP) != 0)
                            ctx.group = context;
                        if ((types & remidy::PER_NOTE_CONTROLLER_PER_EXTRA) != 0)
                            ctx.extra = context;
                        cb(fromRemidyFlags(types), ctx, parameterIndex, value);
                    });
            }

            void removePerNoteControllerListener(ParameterListenerId id) {
                if (!support_)
                    return;
                support_->perNoteControllerChangeEvent().removeListener(id);
            }

            ParameterValueStatus setParameterValue(uint32_t index, double value, uint64_t timestamp) {
                if (!support_)
                    return ParameterValueStatus::BackendError;
                return toParameterValueStatus(support_->setParameter(index, value, timestamp));
            }

            ParameterValueStatus getParameterValue(uint32_t index, double& value) const {
                if (!support_)
                    return ParameterValueStatus::BackendError;
                double out{};
                auto status = support_->getParameter(index, &out);
                if (status == remidy::StatusCode::OK)
                    value = out;
                return toParameterValueStatus(status);
            }

            ParameterValueStatus setPerNoteControllerValue(PerNoteContext context, uint32_t index, double value, uint64_t timestamp) {
                if (!support_)
                    return ParameterValueStatus::BackendError;
                return toParameterValueStatus(support_->setPerNoteController(toRemidyContext(context), index, value, timestamp));
            }

            ParameterValueStatus getPerNoteControllerValue(PerNoteContext context, uint32_t index, double& value) const {
                if (!support_)
                    return ParameterValueStatus::BackendError;
                double out{};
                auto status = support_->getPerNoteController(toRemidyContext(context), index, &out);
                if (status == remidy::StatusCode::OK)
                    value = out;
                return toParameterValueStatus(status);
            }

            double normalizedParameterValue(uint32_t index, double plainValue) const {
                if (!support_)
                    return plainValue;
                auto& params = support_->parameters();
                if (index >= params.size())
                    return plainValue;
                auto* param = params[index];
                if (!param)
                    return plainValue;
                return param->normalizedValue(plainValue);
            }

            double normalizedPerNoteControllerValue(PerNoteContextFlags flags, PerNoteContext context, uint32_t index, double plainValue) const {
                if (!support_)
                    return plainValue;
                auto controllers = support_->perNoteControllers(toRemidyFlags(flags), toRemidyContext(context));
                if (index >= controllers.size())
                    return plainValue;
                auto* param = controllers[index];
                if (!param)
                    return plainValue;
                return param->normalizedValue(plainValue);
            }

            std::vector<ParameterMetadata> perNoteControllerMetadata(PerNoteContextFlags flags, PerNoteContext context) const {
                std::vector<ParameterMetadata> ret{};
                if (!support_)
                    return ret;
                auto controllers = support_->perNoteControllers(toRemidyFlags(flags), toRemidyContext(context));
                ret.reserve(controllers.size());
                for (auto* param : controllers)
                    ret.emplace_back(toParameterMetadata(param));
                return ret;
            }

        private:
            remidy::PluginParameterSupport* support_{nullptr};
        };

        class RemidyAudioBusesAdapter {
        public:
            explicit RemidyAudioBusesAdapter(remidy::PluginAudioBuses* buses) : buses_(buses) {}

            bool hasEventInputs() const { return buses_ && buses_->hasEventInputs(); }
            bool hasEventOutputs() const { return buses_ && buses_->hasEventOutputs(); }

            AudioBusList audioInputBuses() const { return convert(buses_ ? buses_->audioInputBuses() : empty_); }
            AudioBusList audioOutputBuses() const { return convert(buses_ ? buses_->audioOutputBuses() : empty_); }

            int32_t mainInputBusIndex() const { return buses_ ? buses_->mainInputBusIndex() : -1; }
            int32_t mainOutputBusIndex() const { return buses_ ? buses_->mainOutputBusIndex() : -1; }

        private:
            AudioBusList convert(const std::vector<remidy::AudioBusConfiguration*>& buses) const {
                AudioBusList list;
                list.reserve(buses.size());
                for (auto* bus : buses) {
                    if (!bus)
                        continue;
                    list.push_back(AudioBusDescriptor{
                        .role = bus->role() == remidy::AudioBusRole::Main ? AudioBusRole::Main : AudioBusRole::Aux,
                        .channels = bus->channelLayout().channels(),
                        .enabled = bus->enabled()
                    });
                }
                return list;
            }

            remidy::PluginAudioBuses* buses_{nullptr};
            static inline std::vector<remidy::AudioBusConfiguration*> empty_{};
        };
    }
    int32_t instanceIdSerial{0};

    class RemidyAudioPluginInstance : public AudioPluginInstanceAPI {
        bool bypassed_{true};
        std::shared_ptr<remidy_tooling::PluginInstancing> instancing{};
        remidy::PluginInstance* instance{};
        remidy::PluginUISupport* ui_support{nullptr};
        bool uiCreated{false};
        bool uiVisible{false};
        bool uiFloating{true};

        std::unique_ptr<UapmdUmpInputMapper> ump_input_mapper{};
        std::unique_ptr<UapmdUmpOutputMapper> ump_output_mapper{};
        ParameterSupportView parameter_support_view_{};
        AudioBusesView audio_buses_view_{};

        remidy::PluginUISupport* ensureUISupport() {
            if (!instance)
                return nullptr;
            if (!ui_support)
                ui_support = instance->ui();
            return ui_support;
        }

    public:
        explicit RemidyAudioPluginInstance(const std::shared_ptr<remidy_tooling::PluginInstancing>& instancing, remidy::PluginInstance* instance)
          : instancing(instancing), instance(instance) {
            ump_input_mapper = std::make_unique<UapmdNodeUmpInputMapper>(this);
            if (instance) {
                if (auto* parameterSupport = instance->parameters())
                    parameter_support_view_ = ParameterSupportView(std::make_shared<RemidyParameterSupportAdapter>(parameterSupport));
                if (auto* buses = instance->audioBuses())
                    audio_buses_view_ = AudioBusesView(std::make_shared<RemidyAudioBusesAdapter>(buses));
            }
            bypassed_ = false;
        }
        ~RemidyAudioPluginInstance() override {
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
        void bypassed(bool value) override { bypassed_ = value; }

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

        uapmd_status_t processAudio(AudioProcessContext &process) override {
            if (bypassed_)
                return 0;

            const bool replacing = instance && instance->requiresReplacingProcess();
            if (replacing) {
                process.copyInputsToOutputs();
                process.enableReplacingIO();
            }

            if (const auto m = ump_input_mapper.get())
                m->process(process);

            // FIXME: define error codes
            uapmd_status_t status = 0;
            if (const auto p = instance)
                status = static_cast<uapmd_status_t>(p->process(process));

            if (replacing)
                process.disableReplacingIO();
            return status;
        }

        std::vector<uapmd::ParameterMetadata> parameterMetadataList() override {
            std::vector<ParameterMetadata> ret{};
            if (!instance)
                return ret;
            auto pl = instance->parameters();
            for (auto* p : pl->parameters())
                ret.emplace_back(toParameterMetadata(p));
            return ret;
        }
        std::vector<ParameterMetadata> perNoteControllerMetadataList(PerNoteContextFlags contextFlags, PerNoteContext context) override {
            std::vector<ParameterMetadata> ret{};
            if (!instance)
                return ret;
            auto pl = instance->parameters();
            auto controllers = pl->perNoteControllers(toRemidyFlags(contextFlags), toRemidyContext(context));
            for (auto* p : controllers)
                ret.emplace_back(toParameterMetadata(p));
            return ret;
        }
        std::vector<uapmd::PresetsMetadata> presetMetadataList() override {
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

        void loadState(std::vector<uint8_t> &state) override {
            instance->states()->setState(state, remidy::PluginStateSupport::StateContextType::Project, false);
        }

        void loadPreset(int32_t presetIndex) override {
            instance->presets()->loadPreset(presetIndex);
        }

        std::vector<uint8_t> saveState() override {
            return instance->states()->getState(remidy::PluginStateSupport::StateContextType::Project, false);
        }

        double getParameterValue(int32_t index) override {
            double value;
            instance->parameters()->getParameter(index, &value);
            return value;
        }

        void setParameterValue(int32_t index, double value) override {
            instance->parameters()->setParameter(index, value, 0);
        }

        std::string getParameterValueString(int32_t index, double value) override {
            return instance->parameters()->valueToString(index, value);
        }

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override {
            instance->parameters()->setPerNoteController({.note = note }, index, value, 0);
        }

        std::string getPerNoteControllerValueString(uint8_t note, uint8_t index, double value) override {
            return instance->parameters()->valueToStringPerNote({ .note = note }, index, value);
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


        ParameterSupportView parameterSupport() const override {
            return parameter_support_view_;
        }

        AudioBusesView audioBuses() const override {
            return audio_buses_view_;
        }

        void assignMidiDeviceToPlugin(MidiIOFeature* device) override {
            if (!device)
                return;
            ump_output_mapper = std::make_unique<UapmdNodeUmpOutputMapper>(device, this);
        }

        void clearMidiDeviceFromPlugin() override {
            ump_output_mapper.reset();
        }

        bool requiresReplacingProcess() const override {
            return instance && instance->requiresReplacingProcess();
        }
    };
}

std::unique_ptr<uapmd::AudioPluginHostingAPI> uapmd::AudioPluginHostingAPI::create() {
    return std::make_unique<RemidyAudioPluginHost>();
}

uapmd::RemidyAudioPluginHost::RemidyAudioPluginHost() {
#if _WIN32
    // VST3 plugins (especially NI and JUCE-based ones) use COM and require STA.
    // Initialize COM before any DLL loading so initDll / GetPluginFactory run
    // inside a properly-initialized apartment.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr))
        comInitialized = true;
    else if (hr == RPC_E_CHANGED_MODE)
        remidy::Logger::global()->logWarning("RemidyAudioPluginHost: COM already initialized with a different apartment model; VST3 plugins using COM (e.g. NI) may crash");
#endif
    scanning.performPluginScanning(true);
    if (!exists(scanning.pluginListCacheFile()))
        scanning.savePluginListCache();
}

uapmd::RemidyAudioPluginHost::~RemidyAudioPluginHost() {
#if _WIN32
    if (comInitialized)
        CoUninitialize();
#endif
}

std::vector<uapmd::PluginCatalogEntryInfo> uapmd::RemidyAudioPluginHost::pluginCatalogEntries() {
    std::vector<PluginCatalogEntryInfo> ret{};
    for (const auto e : scanning.catalog.getPlugins()) {
        PluginCatalogEntryInfo info{
            .format = e->format(),
            .pluginId = e->pluginId(),
            .displayName = e->displayName(),
            .vendorName = e->vendorName(),
            .productUrl = e->productUrl(),
            .bundlePath = e->bundlePath()
        };
        ret.emplace_back(std::move(info));
    }
    return ret;
}

void uapmd::RemidyAudioPluginHost::savePluginCatalogToFile(std::filesystem::path path) {
    scanning.catalog.save(path);
}

std::filesystem::path empty_path{""};
void uapmd::RemidyAudioPluginHost::performPluginScanning(bool rescan) {
    scanning.catalog.clear();
    if (rescan) {
        scanning.performPluginScanning(false, empty_path);
    }
    else
        scanning.performPluginScanning(false);
}

void uapmd::RemidyAudioPluginHost::createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &formatName, std::string &pluginId, std::function<void(int32_t instanceId, std::string error)>&& callback) {
    auto format = *(scanning.formats() | std::views::filter([formatName](auto f) { return f->name() == formatName; })).begin();
    auto plugins = scanning.catalog.getPlugins();
    auto entry = std::ranges::find_if(plugins, [&formatName,&pluginId](auto e) {
        return e->format() == formatName && e->pluginId() == pluginId;
    });
    if (entry == plugins.end())
        callback(-1, "Plugin not found");
    else {
        auto instancing = std::make_shared<remidy_tooling::PluginInstancing>(scanning, format, *entry);
        auto& request = instancing->configurationRequest();
        request.sampleRate = static_cast<uint32_t>(sampleRate);
        if (inputChannels > 0)
            request.mainInputChannels = inputChannels;
        else
            request.mainInputChannels.reset();
        if (outputChannels > 0)
            request.mainOutputChannels = outputChannels;
        else
            request.mainOutputChannels.reset();
        request.offlineMode = offlineMode;
        auto cb = std::move(callback);
        instancing->makeAlive([this,instancing,cb](std::string error) {
            if (error.empty())
                instancing->withInstance([this,instancing,cb](remidy::PluginInstance* instance) {
                    auto instanceId = instanceIdSerial++;
                    auto api = std::make_unique<RemidyAudioPluginInstance>(instancing, instance);
                    instances[instanceId] = std::move(api);
                    cb(instanceId, "");
                });
            else {
                cb(-1, error);
            }
        });
    }
}

void uapmd::RemidyAudioPluginHost::deletePluginInstance(int32_t instanceId) {
    instances.erase(instanceId);
}
std::vector<int32_t> uapmd::RemidyAudioPluginHost::instanceIds() {
    std::vector<int32_t> ret;
    for (auto& i : instances)
        ret.push_back(i.first);
    return ret;
}

uapmd::AudioPluginInstanceAPI * uapmd::RemidyAudioPluginHost::getInstance(int32_t instanceId) {
    const auto &i = instances[instanceId];
    return i ? i.get() : nullptr;
}

uapmd_status_t uapmd::RemidyAudioPluginHost::processAudio(std::vector<remidy::AudioProcessContext *> contexts) {
    return 0;
}
