
#include "uapmd/uapmd.hpp"


#include <cassert>

namespace uapmd {

    class AudioPluginGraph::Impl {
        std::vector<std::unique_ptr<AudioPluginNode>> nodes;

    public:
        uapmd_status_t appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode);
        int32_t processAudio(AudioProcessContext& process);
    };

    int32_t AudioPluginGraph::Impl::processAudio(AudioProcessContext& process) {
        for (auto& node : nodes)
            node->processAudio(process);
        // FIXME: define return codes
        return 0;
    }

    uapmd_status_t AudioPluginGraph::Impl::appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode) {
        nodes.emplace_back(std::move(newNode));
        // FIXME: define return codes
        return 0;
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

}
