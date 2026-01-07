
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/node-graph/AudioPluginGraph.hpp"


#include <algorithm>
#include <cassert>

namespace uapmd {

    class AudioPluginGraph::Impl {
        std::vector<std::unique_ptr<AudioPluginNode>> nodes;

    public:
        uapmd_status_t appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode);
        bool removePluginInstance(int32_t instanceId);
        int32_t processAudio(AudioProcessContext& process);
        std::vector<AudioPluginNode *> plugins();
    };

    int32_t AudioPluginGraph::Impl::processAudio(AudioProcessContext& process) {
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

    uapmd_status_t AudioPluginGraph::Impl::appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode) {
        nodes.emplace_back(std::move(newNode));
        // FIXME: define return codes
        return 0;
    }

    bool AudioPluginGraph::Impl::removePluginInstance(int32_t instanceId) {
        auto it = std::find_if(nodes.begin(), nodes.end(), [instanceId](const std::unique_ptr<AudioPluginNode>& node) {
            return node && node->instanceId() == instanceId;
        });
        if (it == nodes.end()) {
            return false;
        }
        nodes.erase(it);
        return true;
    }

    std::vector<AudioPluginNode *> AudioPluginGraph::Impl::plugins() {
        std::vector<AudioPluginNode*> ret{};
        for (auto& p : nodes)
            ret.emplace_back(p.get());
        return ret;
    }

    AudioPluginGraph::AudioPluginGraph() {
        impl = new Impl();
    }

    AudioPluginGraph::~AudioPluginGraph() {
        delete impl;
    }

    int32_t AudioPluginGraph::processAudio(AudioProcessContext& process) {
        return impl->processAudio(process);
    }

    uapmd_status_t AudioPluginGraph::appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode) {
        return impl->appendNodeSimple(std::move(newNode));
    }

    bool AudioPluginGraph::removePluginInstance(int32_t instanceId) {
        return impl->removePluginInstance(instanceId);
    }

    std::vector<AudioPluginNode *> AudioPluginGraph::plugins() {
        return impl->plugins();
    }

}
