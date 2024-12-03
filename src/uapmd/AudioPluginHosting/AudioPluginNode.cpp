
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/plugingraph/AudioPluginNode.hpp"


#include <string>

namespace uapmd {

    class AudioPluginNode::Impl {
        std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> node;
    public:
        bool bypassed{false};
        int32_t instanceId;
        explicit Impl(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL, int32_t instanceId) : node(std::move(nodePAL)), instanceId(instanceId) {
        }

        AudioPluginHostPAL::AudioPluginNodePAL* pal() { return node.get(); }
        uapmd_status_t processAudio(AudioProcessContext &process);
    };

    bool AudioPluginNode::bypassed() { return impl->bypassed; }

    void AudioPluginNode::bypassed(bool value) { impl->bypassed = value;; }

    AudioPluginNode::AudioPluginNode(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL, int32_t instanceId) {
        impl = new Impl(std::move(nodePAL), instanceId);
    }

    AudioPluginNode::~AudioPluginNode() {
        delete impl;
    }

    AudioPluginHostPAL::AudioPluginNodePAL* AudioPluginNode::pal() { return impl->pal(); }

    uapmd_status_t AudioPluginNode::processAudio(AudioProcessContext &process) {
        return impl->processAudio(process);
    }

    int32_t AudioPluginNode::instanceId() {
        return impl->instanceId;
    }

    uapmd_status_t AudioPluginNode::Impl::processAudio(AudioProcessContext &process) {
        return pal()->processAudio(process);
    }
}
