
#include "RemidyAudioPluginHostPAL.hpp"
#include <ranges>

namespace uapmd {
    class RemidyAudioPluginNodePAL : public RemidyAudioPluginHostPAL::AudioPluginNodePAL {
        remidy_tooling::PluginInstancing* instancing{};
        remidy::PluginInstance* instance{};
    public:
        explicit RemidyAudioPluginNodePAL(remidy_tooling::PluginInstancing* instancing, remidy::PluginInstance* instance) :
            instancing(instancing), instance(instance) {}
        ~RemidyAudioPluginNodePAL() override {
            delete instancing;
        }

        uapmd_status_t processAudio(AudioProcessContext &process) override {
            // FIXME: define error codes
            return (uapmd_status_t) instance->process(process);
        }
        std::vector<uapmd::ParameterMetadata> parameterMetadataList() override {
            std::vector<ParameterMetadata> ret{};
            auto pl = instance->parameters();
            for (auto p : pl->parameters())
                ret.emplace_back(ParameterMetadata {
                    .id = p->id(),
                    .name = p->name(),
                    .path = p->path(),
                    .initialValue = p->defaultValue(),
                    .hidden = p->hidden()
                });
            return ret;
        }

        std::string& formatName() const override { return instance->info()->format(); }
        std::string& pluginId() const override { return instance->info()->pluginId(); }
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
    auto format = *(scanning.formats | std::views::filter([formatName](auto f) { return f->name() == formatName; })).begin();
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
