
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/plugingraph/AudioPluginNode.hpp"


#include <string>

namespace uapmd {

    AudioPluginNode::AudioPluginNode(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL, int32_t instanceId)
        : node_(std::move(nodePAL)), instance_id_(instanceId) {}

    AudioPluginNode::~AudioPluginNode() = default;

    bool AudioPluginNode::bypassed() { return bypassed_; }

    void AudioPluginNode::bypassed(bool value) { bypassed_ = value; }

    AudioPluginHostPAL::AudioPluginNodePAL* AudioPluginNode::pal() { return node_.get(); }

    uapmd_status_t AudioPluginNode::processAudio(AudioProcessContext &process) {
        return pal()->processAudio(process);
    }

    int32_t AudioPluginNode::instanceId() {
        return instance_id_;
    }

    void AudioPluginNode::loadState(std::vector<uint8_t>& state) {
        pal()->loadState(state);
    }

    std::vector<uint8_t> AudioPluginNode::saveState() {
        return pal()->saveState();
    }
}
