
#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"


#include <algorithm>
#include <cassert>

namespace uapmd {

    class AudioPluginGraphImpl : public AudioPluginGraph {
        std::vector<std::unique_ptr<AudioPluginNode>> nodes;

    public:
        uapmd_status_t appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode) override;
        bool removePluginInstance(int32_t instanceId) override;
        int32_t processAudio(AudioProcessContext& process) override;
        std::vector<AudioPluginNode *> plugins() override;
    };

    int32_t AudioPluginGraphImpl::processAudio(AudioProcessContext& process) {
        if (nodes.empty())
            return 0;

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto status = nodes[i]->processAudio(process);
            if (status != 0)
                return status;
            if (i + 1 < nodes.size()) {
                process.advanceToNextNode();
            }
        }
        return 0;
    }

    uapmd_status_t AudioPluginGraphImpl::appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode) {
        nodes.emplace_back(std::move(newNode));
        // FIXME: define return codes
        return 0;
    }

    bool AudioPluginGraphImpl::removePluginInstance(int32_t instanceId) {
        auto it = std::find_if(nodes.begin(), nodes.end(), [instanceId](const std::unique_ptr<AudioPluginNode>& node) {
            return node && node->instanceId() == instanceId;
        });
        if (it == nodes.end()) {
            return false;
        }
        nodes.erase(it);
        return true;
    }

    std::vector<AudioPluginNode *> AudioPluginGraphImpl::plugins() {
        std::vector<AudioPluginNode*> ret{};
        for (auto& p : nodes)
            ret.emplace_back(p.get());
        return ret;
    }

    std::unique_ptr<AudioPluginGraph> AudioPluginGraph::create() {
        return std::make_unique<AudioPluginGraphImpl>();
    }

}
