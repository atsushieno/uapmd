
#include "RemidyAudioPluginHost.hpp"
#include <functional>
#include <ranges>

#include "remidy-tooling/PluginInstancing.hpp"
#include "../../uapmd/Midi/UapmdNodeUmpMapper.hpp"

namespace uapmd {
    int32_t instanceIdSerial{0};

    class RemidyAudioPluginInstance : public AudioPluginInstanceAPI {
        std::function<void()> on_delete_instance;

        bool bypassed_{true};
        std::shared_ptr<remidy_tooling::PluginInstancing> instancing{};
        remidy::PluginInstance* instance{};
        remidy::PluginUISupport* ui_support{nullptr};
        bool uiCreated{false};
        bool uiVisible{false};
        bool uiFloating{true};

        std::unique_ptr<UapmdUmpInputMapper> ump_input_mapper{};
        std::unique_ptr<UapmdUmpOutputMapper> ump_output_mapper{};

        remidy::PluginUISupport* ensureUISupport() {
            if (!instance)
                return nullptr;
            if (!ui_support)
                ui_support = instance->ui();
            return ui_support;
        }

    public:
        explicit RemidyAudioPluginInstance(const std::shared_ptr<remidy_tooling::PluginInstancing>& instancing, remidy::PluginInstance* instance, const std::function<void()>&& onDeleteInstance)
          : on_delete_instance(onDeleteInstance), instancing(instancing), instance(instance) {
            ump_input_mapper = std::make_unique<UapmdNodeUmpInputMapper>(this);
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
            on_delete_instance();
        }

        void bypassed(bool value) override {
            bypassed_ = value;
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


        remidy::PluginParameterSupport* parameterSupport() override {
            if (!instance)
                return nullptr;
            return instance->parameters();
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

uapmd::AudioPluginHostingAPI* uapmd::RemidyAudioPluginHost::instance() {
    static RemidyAudioPluginHost impl{};
    return &impl;
}

uapmd::RemidyAudioPluginHost::RemidyAudioPluginHost() {
    scanning.performPluginScanning();
    if (!exists(scanning.pluginListCacheFile()))
        scanning.savePluginListCache();
}

std::filesystem::path empty_path{""};
void uapmd::RemidyAudioPluginHost::performPluginScanning(bool rescan) {
    catalog().clear();
    if (rescan) {
        scanning.performPluginScanning(empty_path);
    }
    else
        scanning.performPluginScanning();
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
                    auto api = std::make_unique<RemidyAudioPluginInstance>(instancing, instance, [&,instanceId] {
                        deletePluginInstance(instanceId);
                    });
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
