
#include <string>
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/plugingraph/AudioPluginNode.hpp"
#include "UapmdNodeUmpMapper.hpp"

namespace uapmd {

    AudioPluginNode::AudioPluginNode(
        std::unique_ptr<UapmdUmpInputMapper> umpInputMapper,
        std::unique_ptr<UapmdUmpOutputMapper> umpOutputMapper,
        std::unique_ptr<AudioPluginNodePAL> nodePAL,
        int32_t instanceId
    ) : node_(std::move(nodePAL)),
        instance_id_(instanceId),
        ump_input_mapper(std::move(umpInputMapper)),
        ump_output_mapper(std::move(umpOutputMapper)) {
    }

    AudioPluginNode::~AudioPluginNode() = default;

    bool AudioPluginNode::bypassed() { return bypassed_; }

    void AudioPluginNode::bypassed(bool value) { bypassed_ = value; }

    AudioPluginNodePAL* AudioPluginNode::pal() { return node_.get(); }

    uapmd_status_t AudioPluginNode::processAudio(AudioProcessContext &process) {
        // FIXME: assign timestamp
        ump_input_mapper->process(0, process);
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

    void AudioPluginNode::setUmpOutputMapper(std::unique_ptr<UapmdUmpOutputMapper> mapper) {
        ump_output_mapper = std::move(mapper);
    }
}
