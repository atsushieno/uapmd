
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
        std::function<bool(uint32_t, uint32_t)> resizeHandler{};

        remidy::PluginUISupport* ensureUISupport() {
            if (!instance)
                return nullptr;
            if (!ui_support)
                ui_support = instance->ui();
            return ui_support;
        }

        void applyResizeHandler() {
            auto ui = ensureUISupport();
            if (!ui)
                return;
            ui->setResizeRequestHandler(resizeHandler);
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
                        .initialValue = p->defaultValue(),
                        .minValue = p->minValue(),
                        .maxValue = p->maxValue(),
                        .hidden = p->hidden(),
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

        bool hasUISupport() override {
            return ensureUISupport() != nullptr;
        }

        bool createUI(bool isFloating) override {
            auto ui = ensureUISupport();
            if (!ui)
                return false;
            if (uiCreated && uiFloating == isFloating)
                return true;
            if (uiCreated) {
                if (uiVisible)
                    ui->hide();
                ui->destroy();
                uiCreated = false;
                uiVisible = false;
            }
            if (!ui->create(isFloating))
                return false;
            uiCreated = true;
            uiFloating = isFloating;
            applyResizeHandler();
            return true;
        }

        bool attachUI(void* parentHandle) override {
            auto ui = ensureUISupport();
            if (!ui || !uiCreated || uiFloating)
                return false;
            if (!parentHandle)
                return false;
            return ui->attachToParent(parentHandle);
        }

        bool showUI() override {
            auto ui = ensureUISupport();
            if (!ui)
                return false;
            if (!uiCreated && !createUI(true))
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

        void setUIResizeHandler(std::function<bool(uint32_t, uint32_t)> handler) override {
            resizeHandler = std::move(handler);
            applyResizeHandler();
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
    };
}

uapmd::RemidyAudioPluginHostPAL::RemidyAudioPluginHostPAL() {
    scanning.performPluginScanning();
}

std::filesystem::path empty_path{""};
void uapmd::RemidyAudioPluginHostPAL::performPluginScanning(bool rescan) {
    if (rescan) {
        catalog().clear();
        scanning.performPluginScanning(empty_path);
    }
    else
        scanning.performPluginScanning();
}

int32_t instanceIdSerial{0};

void uapmd::RemidyAudioPluginHostPAL::createPluginInstance(uint32_t sampleRate, std::string &formatName, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode> node, std::string error)>&& callback) {
    scanning.performPluginScanning();
    auto format = *(scanning.formats() | std::views::filter([formatName](auto f) { return f->name() == formatName; })).begin();
    auto plugins = scanning.catalog.getPlugins();
    auto entry = *(plugins | std::views::filter([formatName,pluginId](auto e) { return e->format() == formatName && e->pluginId() == pluginId; })).begin();
    if (entry == nullptr)
        callback(nullptr, "Plugin not found");
    else {
        auto instancing = new remidy_tooling::PluginInstancing(scanning, format, entry);
        instancing->configurationRequest().sampleRate = (uint32_t) sampleRate;
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
