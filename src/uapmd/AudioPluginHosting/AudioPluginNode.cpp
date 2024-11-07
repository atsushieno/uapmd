
#include "uapmd/uapmd.hpp"


#include <string>

namespace uapmd {

    class AudioPluginNode::Impl {
        std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> node;
    public:
        explicit Impl(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL) : node(std::move(nodePAL)) {
        }
        bool bypassed;

        AudioPluginHostPAL::AudioPluginNodePAL* pal() { return node.get(); }
        uapmd_status_t processAudio(AudioProcessContext &process);
    };

    bool AudioPluginNode::bypassed() { return impl->bypassed; }

    void AudioPluginNode::bypassed(bool value) { impl->bypassed = value;; }

    AudioPluginNode::AudioPluginNode(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL) {
        impl = new Impl(std::move(nodePAL));
    }

    AudioPluginNode::~AudioPluginNode() {
        delete impl;
    }

    AudioPluginHostPAL::AudioPluginNodePAL* AudioPluginNode::pal() { return impl->pal(); }

    uapmd_status_t AudioPluginNode::processAudio(AudioProcessContext &process) {
        return impl->processAudio(process);
    }

    uapmd_status_t AudioPluginNode::Impl::processAudio(AudioProcessContext &process) {
        return pal()->processAudio(process);
    }
}
