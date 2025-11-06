
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
        void loadState(std::vector<uint8_t>& state);
        std::vector<uint8_t> saveState();
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

    void AudioPluginNode::loadState(std::vector<uint8_t>& state) {
        impl->loadState(state);
    }

    std::vector<uint8_t> AudioPluginNode::saveState() {
        return impl->saveState();
    }

    void AudioPluginNode::setOfflineMode(bool offlineMode) {
        auto* palPtr = impl->pal();
        if (palPtr)
            palPtr->setOfflineMode(offlineMode);
    }

    uapmd_status_t AudioPluginNode::Impl::processAudio(AudioProcessContext &process) {
        return pal()->processAudio(process);
    }

    void AudioPluginNode::Impl::loadState(std::vector<uint8_t>& state) {
        pal()->loadState(state);
    }

    std::vector<uint8_t> AudioPluginNode::Impl::saveState() {
        return pal()->saveState();
    }
}
