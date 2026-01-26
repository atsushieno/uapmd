
#include <string>
#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    AudioPluginNode::AudioPluginNode(
        std::unique_ptr<AudioPluginInstanceAPI> nodePAL,
        int32_t instanceId
    ) : node_(std::move(nodePAL)),
        instance_id_(instanceId) {
    }

    AudioPluginNode::~AudioPluginNode() = default;

    bool AudioPluginNode::bypassed() { return bypassed_; }

    void AudioPluginNode::bypassed(bool value) { bypassed_ = value; }

    AudioPluginInstanceAPI* AudioPluginNode::pal() { return node_.get(); }

    uapmd_status_t AudioPluginNode::processAudio(AudioProcessContext &process) {
        if (bypassed_)
            // FIXME: maybe switch to remidy::StatusCode?
            return 0;
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

    void AudioPluginNode::setUmpInputMapper(std::unique_ptr<UapmdUmpInputMapper> mapper) {
        if (mapper)
            ump_input_mapper = std::move(mapper);
        else
            ump_input_mapper.reset();
    }

    void AudioPluginNode::setUmpOutputMapper(std::unique_ptr<UapmdUmpOutputMapper> mapper) {
        if (mapper)
            ump_output_mapper = std::move(mapper);
        else
            ump_output_mapper.reset();
    }
}
