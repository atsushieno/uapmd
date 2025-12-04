
#include "RemidyAudioPluginHostPAL.hpp"
#include <functional>
#include <ranges>

namespace uapmd {
    class RemidyAudioPluginNodePAL : public RemidyAudioPluginHostPAL::AudioPluginNodePAL {
        remidy_tooling::PluginInstancing* instancing{};
        remidy::PluginInstance* instance{};
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
        explicit RemidyAudioPluginNodePAL(remidy_tooling::PluginInstancing* instancing, remidy::PluginInstance* instance) :
            instancing(instancing), instance(instance) {}
        ~RemidyAudioPluginNodePAL() override {
            if (ui_support) {
                if (uiVisible)
                    ui_support->hide();
                if (uiCreated)
                    ui_support->destroy();
                uiVisible = false;
                uiCreated = false;
                uiFloating = true;
            }
            delete instancing;
        }

        uapmd_status_t processAudio(AudioProcessContext &process) override {
            // FIXME: define error codes
            return (uapmd_status_t) instance->process(process);
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
                        .namedValues = enums
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

        void setParameterValue(int32_t index, double value) override {
            instance->parameters()->setParameter(index, value, 0);
        }

        std::string getParameterValueString(int32_t index, double value) override {
            return instance->parameters()->valueToString(index, value);
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
    };
}

uapmd::RemidyAudioPluginHostPAL::RemidyAudioPluginHostPAL() {
    scanning.performPluginScanning();
}

std::filesystem::path empty_path{""};
void uapmd::RemidyAudioPluginHostPAL::performPluginScanning(bool rescan) {
    catalog().clear();
    if (rescan) {
        scanning.performPluginScanning(empty_path);
    }
    else
        scanning.performPluginScanning();
}

int32_t instanceIdSerial{0};

void uapmd::RemidyAudioPluginHostPAL::createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &formatName, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode> node, std::string error)>&& callback) {
    scanning.performPluginScanning();
    auto format = *(scanning.formats() | std::views::filter([formatName](auto f) { return f->name() == formatName; })).begin();
    auto plugins = scanning.catalog.getPlugins();
    auto entry = *(plugins | std::views::filter([formatName,pluginId](auto e) { return e->format() == formatName && e->pluginId() == pluginId; })).begin();
    if (entry == nullptr)
        callback(nullptr, "Plugin not found");
    else {
        auto instancing = new remidy_tooling::PluginInstancing(scanning, format, entry);
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
        instancing->makeAlive([instancing,cb](std::string error) {
            if (error.empty())
                instancing->withInstance([instancing,cb](auto instance) {
                    auto node = std::make_unique<AudioPluginNode>(std::make_unique<RemidyAudioPluginNodePAL>(instancing, instance), instanceIdSerial++);
                    cb(std::move(node), "");
                });
            else {
                cb(nullptr, error);
                delete instancing;
            }
        });
    }
}


uapmd_status_t uapmd::RemidyAudioPluginHostPAL::processAudio(std::vector<remidy::AudioProcessContext *> contexts) {
    return 0;
}
