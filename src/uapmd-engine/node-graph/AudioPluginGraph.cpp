
#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    class AudioPluginGraphImpl : public AudioPluginGraph {
        struct InstanceInfo {
            int32_t instance_id;
            AudioPluginInstanceAPI* instance;
            std::function<void()> on_delete;
        };
        std::vector<InstanceInfo> instances;

    public:
        uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) override;
        bool removeNodeSimple(int32_t instanceId) override;
        int32_t processAudio(AudioProcessContext& process) override;
        std::map<int32_t, AudioPluginInstanceAPI*> plugins() override;
    };

    int32_t AudioPluginGraphImpl::processAudio(AudioProcessContext& process) {
        if (instances.empty())
            return 0;

        for (size_t i = 0; i < instances.size(); ++i) {
            auto status = instances[i].instance->processAudio(process);
            if (status != 0)
                return status;
            if (i + 1 < instances.size()) {
                process.advanceToNextNode();
            }
        }
        return 0;
    }

    uapmd_status_t AudioPluginGraphImpl::appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) {
        const InstanceInfo info{instanceId, instance, onDelete};
        instances.push_back(info);
        // FIXME: define return codes
        return 0;
    }

    bool AudioPluginGraphImpl::removeNodeSimple(int32_t instanceId) {
        for (size_t i = 0; i < instances.size(); ++i) {
            if (instances[i].instance_id == instanceId) {
                instances[i].on_delete();
                instances.erase(instances.begin() + i);
                return true;
            }
        }
        return false;
    }

    std::map<int32_t, AudioPluginInstanceAPI*> AudioPluginGraphImpl::plugins() {
        std::map<int32_t, AudioPluginInstanceAPI*> ret{};
        for (auto & instance : instances) {
            ret[instance.instance_id] = instance.instance;
        }
        return ret;
    }

    std::unique_ptr<AudioPluginGraph> AudioPluginGraph::create() {
        return std::make_unique<AudioPluginGraphImpl>();
    }

}
